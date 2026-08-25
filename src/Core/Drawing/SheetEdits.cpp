#include "Core/Drawing/SheetEdits.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <variant>

namespace paramcad {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kTiny = 1e-9;

// WHERE ALONG A LINE A POINT SITS, as 0 at `a` and 1 at `b`.
//
// Everything trim and extend do is done in this one number. Working in
// coordinates instead means every comparison is two comparisons that have to
// agree, and the one place they disagree is the point exactly on an end.
double ParameterAlong(Vec2 a, Vec2 b, Vec2 point) noexcept {
    const Vec2 along{b.x - a.x, b.y - a.y};
    const double lengthSquared = along.x * along.x + along.y * along.y;
    if (lengthSquared < kTiny) return 0.0;
    return ((point.x - a.x) * along.x + (point.y - a.y) * along.y) / lengthSquared;
}

Vec2 PointAt(Vec2 a, Vec2 b, double t) noexcept {
    return Vec2{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

// Every place `victim` meets `cutter`, as parameters along the victim. Points
// beyond the victim's own ends are dropped: a cutter that crosses the line
// this one lies ON, but not this one, cuts nothing.
void CrossingsAlongLine(Vec2 a, Vec2 b, const DrawShape& cutter, std::vector<double>& into) {
    const auto record = [&](Vec2 at) {
        const double t = ParameterAlong(a, b, at);
        if (t > kTiny && t < 1.0 - kTiny) into.push_back(t);
    };

    if (const auto* line = std::get_if<DrawLine>(&cutter)) {
        // BOUNDED BOTH WAYS. A cutter is a real edge on the paper, not the
        // infinite line through it -- trimming to a segment that stops short
        // would cut where nothing is drawn.
        if (const std::optional<Vec2> at =
                LineLineIntersection(a, b, line->a, line->b, true))
            record(*at);
    } else if (const auto* circle = std::get_if<DrawCircle>(&cutter)) {
        for (const Vec2 at : LineCircleIntersection(a, b, circle->centre, circle->radius, true))
            record(at);
    } else if (const auto* arc = std::get_if<DrawArc>(&cutter)) {
        for (const Vec2 at : LineCircleIntersection(a, b, arc->centre, arc->radius, true)) {
            const double angle = std::atan2(at.y - arc->centre.y, at.x - arc->centre.x);
            // ...and only where the ARC actually is. The rest of that circle
            // is not drawn, and cutting against it would trim at a point the
            // reader cannot see.
            if (AngleWithinArc(angle, arc->startAngle, arc->endAngle)) record(at);
        }
    }
}

// The same for a circle or an arc, as ANGLES.
void CrossingsAroundCircle(Vec2 centre, double radius, const DrawShape& cutter,
                           std::vector<double>& into) {
    const auto record = [&](Vec2 at) {
        into.push_back(NormaliseAngle(std::atan2(at.y - centre.y, at.x - centre.x)));
    };
    if (const auto* line = std::get_if<DrawLine>(&cutter)) {
        for (const Vec2 at : LineCircleIntersection(line->a, line->b, centre, radius, true))
            record(at);
    } else if (const auto* circle = std::get_if<DrawCircle>(&cutter)) {
        for (const Vec2 at :
             CircleCircleIntersection(centre, radius, circle->centre, circle->radius))
            record(at);
    } else if (const auto* arc = std::get_if<DrawArc>(&cutter)) {
        for (const Vec2 at : CircleCircleIntersection(centre, radius, arc->centre, arc->radius)) {
            const double angle = std::atan2(at.y - arc->centre.y, at.x - arc->centre.x);
            if (AngleWithinArc(angle, arc->startAngle, arc->endAngle)) record(at);
        }
    }
}

SheetEditResult Refuse(std::string why) {
    SheetEditResult out;
    out.why = std::move(why);
    return out;
}

SheetEditResult Made(std::vector<DrawShape> shapes) {
    SheetEditResult out;
    out.ok = true;
    out.shapes = std::move(shapes);
    return out;
}

} // namespace

SheetEditResult TrimShape(const DrawShape& victim, const std::vector<DrawShape>& cutters,
                          Vec2 pickedAt) {
    if (cutters.empty()) return Refuse("nothing was chosen to trim against");

    if (const auto* line = std::get_if<DrawLine>(&victim)) {
        std::vector<double> cuts;
        for (const DrawShape& cutter : cutters)
            CrossingsAlongLine(line->a, line->b, cutter, cuts);
        if (cuts.empty()) return Refuse("this line does not cross anything that was chosen");

        std::sort(cuts.begin(), cuts.end());
        // The ends count as boundaries of the pieces, so a line cut once
        // becomes two pieces rather than one piece and a leftover.
        cuts.insert(cuts.begin(), 0.0);
        cuts.push_back(1.0);

        const double picked = ParameterAlong(line->a, line->b, pickedAt);
        std::vector<DrawShape> kept;
        bool droppedOne = false;
        for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
            const double from = cuts[i];
            const double to = cuts[i + 1];
            if (to - from < kTiny) continue;
            // THE PICKED PIECE IS THE ONE THAT GOES. Keeping it instead is the
            // single most common way to write this function backwards, and the
            // result is a drawing that looks edited and is inside out.
            if (picked >= from && picked <= to && !droppedOne) {
                droppedOne = true;
                continue;
            }
            kept.push_back(DrawLine{PointAt(line->a, line->b, from),
                                    PointAt(line->a, line->b, to)});
        }
        if (!droppedOne)
            return Refuse("the point picked is not on the part of this line that was cut");
        return Made(std::move(kept));
    }

    if (const auto* circle = std::get_if<DrawCircle>(&victim)) {
        std::vector<double> cuts;
        for (const DrawShape& cutter : cutters)
            CrossingsAroundCircle(circle->centre, circle->radius, cutter, cuts);
        // A CIRCLE NEEDS TWO CUTS, not one. Cut once it is still a closed
        // circle -- there is no end to remove -- and a tool that returned an
        // arc from one crossing would silently delete half a circle.
        if (cuts.size() < 2)
            return Refuse("a circle has to be cut in two places before a piece can come out");
        std::sort(cuts.begin(), cuts.end());

        const double picked =
            NormaliseAngle(std::atan2(pickedAt.y - circle->centre.y,
                                      pickedAt.x - circle->centre.x));
        std::vector<DrawShape> kept;
        bool droppedOne = false;
        for (std::size_t i = 0; i < cuts.size(); ++i) {
            const double from = cuts[i];
            const double to = cuts[(i + 1) % cuts.size()];
            if (!droppedOne && AngleWithinArc(picked, from, to)) {
                droppedOne = true;
                continue;
            }
            kept.push_back(DrawArc{circle->centre, circle->radius, from, to});
        }
        if (!droppedOne) return Refuse("the point picked is not on this circle");
        return Made(std::move(kept));
    }

    if (const auto* arc = std::get_if<DrawArc>(&victim)) {
        std::vector<double> cuts;
        for (const DrawShape& cutter : cutters)
            CrossingsAroundCircle(arc->centre, arc->radius, cutter, cuts);
        std::vector<double> onTheArc;
        for (const double angle : cuts)
            if (AngleWithinArc(angle, arc->startAngle, arc->endAngle)) onTheArc.push_back(angle);
        if (onTheArc.empty()) return Refuse("this arc does not cross anything that was chosen");

        // Sorted by how far ROUND THE ARC they are, not by absolute angle: an
        // arc that runs through zero has crossings at 350 and 10 degrees, and
        // sorting those numerically puts them in the wrong order.
        const auto swept = [&](double angle) {
            double turn = angle - arc->startAngle;
            while (turn < 0.0) turn += kTwoPi;
            return turn;
        };
        std::sort(onTheArc.begin(), onTheArc.end(),
                  [&](double a, double b) { return swept(a) < swept(b); });
        onTheArc.insert(onTheArc.begin(), arc->startAngle);
        onTheArc.push_back(arc->endAngle);

        const double picked = NormaliseAngle(
            std::atan2(pickedAt.y - arc->centre.y, pickedAt.x - arc->centre.x));
        std::vector<DrawShape> kept;
        bool droppedOne = false;
        for (std::size_t i = 0; i + 1 < onTheArc.size(); ++i) {
            const double from = onTheArc[i];
            const double to = onTheArc[i + 1];
            if (std::fabs(swept(to) - swept(from)) < kTiny) continue;
            if (!droppedOne && AngleWithinArc(picked, from, to)) {
                droppedOne = true;
                continue;
            }
            kept.push_back(DrawArc{arc->centre, arc->radius, from, to});
        }
        if (!droppedOne) return Refuse("the point picked is not on the part of this arc that "
                                       "was cut");
        return Made(std::move(kept));
    }

    return Refuse("this kind of object cannot be trimmed yet");
}

SheetEditResult ExtendShape(const DrawShape& victim, const std::vector<DrawShape>& boundaries,
                            Vec2 pickedAt) {
    const auto* line = std::get_if<DrawLine>(&victim);
    if (line == nullptr) return Refuse("only a line can be extended yet");
    if (boundaries.empty()) return Refuse("nothing was chosen to extend to");

    // WHICH END MOVES: the one the user picked near. They have already said
    // it; asking again, or picking for them, is how a tool ends up growing the
    // wrong end of a line that is nearly symmetrical.
    const double picked = ParameterAlong(line->a, line->b, pickedAt);
    const bool movingB = picked > 0.5;
    const Vec2 fixed = movingB ? line->a : line->b;
    const Vec2 moving = movingB ? line->b : line->a;

    // The candidates are the crossings of the INFINITE line with each
    // boundary, kept only if they lie BEYOND the moving end. A crossing behind
    // it would shorten the line, which is what trim is for.
    double best = 0.0;
    bool found = false;
    const auto consider = [&](Vec2 at) {
        const double t = ParameterAlong(fixed, moving, at);
        if (t <= 1.0 + kTiny) return; // not past the end that is moving
        if (!found || t < best) {
            best = t;
            found = true;
        }
    };
    // A long reach, so the infinite line is represented without a special case.
    const double span = std::hypot(moving.x - fixed.x, moving.y - fixed.y);
    if (span < kTiny) return Refuse("a line of no length has no direction to extend in");
    const Vec2 far{fixed.x + (moving.x - fixed.x) * (1.0 + 1e6 / span),
                   fixed.y + (moving.y - fixed.y) * (1.0 + 1e6 / span)};

    for (const DrawShape& boundary : boundaries) {
        if (const auto* other = std::get_if<DrawLine>(&boundary)) {
            if (const std::optional<Vec2> at =
                    LineLineIntersection(fixed, far, other->a, other->b, true))
                consider(*at);
        } else if (const auto* circle = std::get_if<DrawCircle>(&boundary)) {
            for (const Vec2 at :
                 LineCircleIntersection(fixed, far, circle->centre, circle->radius, true))
                consider(at);
        } else if (const auto* arc = std::get_if<DrawArc>(&boundary)) {
            for (const Vec2 at :
                 LineCircleIntersection(fixed, far, arc->centre, arc->radius, true)) {
                const double angle = std::atan2(at.y - arc->centre.y, at.x - arc->centre.x);
                if (AngleWithinArc(angle, arc->startAngle, arc->endAngle)) consider(at);
            }
        }
    }
    // REFUSED RATHER THAN STRETCHED. A line that grew by some default amount
    // looks exactly like one that reached something.
    if (!found) return Refuse("there is nothing ahead of that end to extend to");

    const Vec2 landed = PointAt(fixed, moving, best);
    return Made({movingB ? DrawLine{line->a, landed} : DrawLine{landed, line->b}});
}

namespace {

// The corner two lines make, and which way each one runs away from it.
struct Corner {
    bool ok = false;
    Vec2 at{};
    // Unit vectors from the corner towards the end each line KEEPS -- decided
    // by the pick, which is the whole reason both picks are arguments.
    Vec2 firstWay{};
    Vec2 secondWay{};
    // ...and the ends themselves. Worked out here rather than by the two
    // callers, because "which end survives" is one question with one answer,
    // and a fillet and a chamfer that answered it differently would round one
    // corner and cut across another.
    Vec2 firstFar{};
    Vec2 secondFar{};
};

Corner CornerOf(const DrawLine& first, const DrawLine& second, Vec2 pickFirst, Vec2 pickSecond) {
    Corner out;
    // The INFINITE lines, because filleting two lines that do not yet meet is
    // ordinary: the corner is where they would meet, and both get extended to
    // it.
    const std::optional<Vec2> at =
        LineLineIntersection(first.a, first.b, second.a, second.b, false);
    if (!at) return out;

    const auto wayTowardsPick = [](const DrawLine& line, Vec2 corner, Vec2 pick) {
        // The kept side is the one the user picked on. Two lines that cross
        // make four corners and all four are valid fillets; the picks are what
        // say which.
        const Vec2 towards{pick.x - corner.x, pick.y - corner.y};
        const double length = std::hypot(towards.x, towards.y);
        if (length < kTiny) {
            // Picked exactly on the corner: fall back to the longer half, so
            // the answer is at least deterministic.
            const double toA = std::hypot(line.a.x - corner.x, line.a.y - corner.y);
            const double toB = std::hypot(line.b.x - corner.x, line.b.y - corner.y);
            const Vec2 end = toA > toB ? line.a : line.b;
            const double span = std::hypot(end.x - corner.x, end.y - corner.y);
            if (span < kTiny) return Vec2{0.0, 0.0};
            return Vec2{(end.x - corner.x) / span, (end.y - corner.y) / span};
        }
        // Projected onto the line's own direction, so a pick slightly off the
        // line still says which way along it the user meant.
        const Vec2 along{line.b.x - line.a.x, line.b.y - line.a.y};
        const double alongLength = std::hypot(along.x, along.y);
        if (alongLength < kTiny) return Vec2{0.0, 0.0};
        const Vec2 unit{along.x / alongLength, along.y / alongLength};
        const double sign = towards.x * unit.x + towards.y * unit.y >= 0.0 ? 1.0 : -1.0;
        return Vec2{unit.x * sign, unit.y * sign};
    };

    out.at = *at;
    out.firstWay = wayTowardsPick(first, *at, pickFirst);
    out.secondWay = wayTowardsPick(second, *at, pickSecond);
    if (std::hypot(out.firstWay.x, out.firstWay.y) < 0.5 ||
        std::hypot(out.secondWay.x, out.secondWay.y) < 0.5)
        return out;

    // The surviving end is the one FURTHEST along the kept direction -- which
    // also handles the case where the corner is off the end of the line and
    // both ends are on the same side: the far one stays and the line is
    // EXTENDED to the corner rather than trimmed, which is what a user
    // filleting two lines that do not yet meet expects.
    const auto furtherAlong = [](const DrawLine& line, Vec2 corner, Vec2 way) {
        const double toA = (line.a.x - corner.x) * way.x + (line.a.y - corner.y) * way.y;
        const double toB = (line.b.x - corner.x) * way.x + (line.b.y - corner.y) * way.y;
        return toA > toB ? line.a : line.b;
    };
    out.firstFar = furtherAlong(first, out.at, out.firstWay);
    out.secondFar = furtherAlong(second, out.at, out.secondWay);
    out.ok = true;
    return out;
}

} // namespace

SheetEditResult FilletLines(const DrawShape& first, const DrawShape& second, Vec2 pickFirst,
                            Vec2 pickSecond, double radiusMm) {
    const auto* a = std::get_if<DrawLine>(&first);
    const auto* b = std::get_if<DrawLine>(&second);
    if (a == nullptr || b == nullptr) return Refuse("a fillet joins two lines");
    if (radiusMm < 0.0) return Refuse("a fillet radius cannot be negative");

    const Corner corner = CornerOf(*a, *b, pickFirst, pickSecond);
    if (!corner.ok) return Refuse("these two lines are parallel, so they make no corner");

    const double cosine =
        corner.firstWay.x * corner.secondWay.x + corner.firstWay.y * corner.secondWay.y;
    if (cosine > 1.0 - 1e-9 || cosine < -1.0 + 1e-9)
        return Refuse("these two lines lie along each other, so there is no corner to round");

    // A RADIUS OF ZERO IS A CORNER, which is what every CAD system does with
    // it and what a user typing 0 means: bring both lines to the point.
    if (radiusMm < kTiny)
        return Made({DrawLine{corner.firstFar, corner.at},
                     DrawLine{corner.at, corner.secondFar}});

    // How far back along each line the arc touches: the tangent length of the
    // half angle between them.
    const double halfAngle = std::acos(std::max(-1.0, std::min(1.0, cosine))) / 2.0;
    const double tangent = std::tan(halfAngle);
    if (tangent < kTiny) return Refuse("this corner is too shallow to round");
    const double setback = radiusMm / tangent;

    // MEASURED FROM THE CORNER TO THE END THAT SURVIVES, not along the whole
    // line: the corner can be off the end of either line, and then what
    // matters is how much line there is on the kept side of it.
    const double firstReach = std::hypot(corner.firstFar.x - corner.at.x,
                                         corner.firstFar.y - corner.at.y);
    const double secondReach = std::hypot(corner.secondFar.x - corner.at.x,
                                          corner.secondFar.y - corner.at.y);
    // REFUSED WHEN IT WILL NOT FIT. A fillet bigger than the lines eats them
    // entirely and leaves an arc floating where a corner was -- valid
    // geometry, and not a drawing of anything.
    if (setback > firstReach + kTiny || setback > secondReach + kTiny)
        return Refuse("a fillet of that radius is bigger than the lines it would join");

    const Vec2 firstTouch{corner.at.x + corner.firstWay.x * setback,
                          corner.at.y + corner.firstWay.y * setback};
    const Vec2 secondTouch{corner.at.x + corner.secondWay.x * setback,
                           corner.at.y + corner.secondWay.y * setback};
    // The arc's centre is out along the bisector, at radius / sin(half angle).
    const Vec2 bisector{corner.firstWay.x + corner.secondWay.x,
                        corner.firstWay.y + corner.secondWay.y};
    const double bisectorLength = std::hypot(bisector.x, bisector.y);
    if (bisectorLength < kTiny) return Refuse("this corner is too shallow to round");
    const double toCentre = radiusMm / std::sin(halfAngle);
    const Vec2 centre{corner.at.x + bisector.x / bisectorLength * toCentre,
                      corner.at.y + bisector.y / bisectorLength * toCentre};

    double from = std::atan2(firstTouch.y - centre.y, firstTouch.x - centre.x);
    double to = std::atan2(secondTouch.y - centre.y, secondTouch.x - centre.x);
    // ARCS ARE COUNTER-CLOCKWISE, always (DrawArc's own rule). Whichever way
    // round the two touches came out, the SHORT way between them is the
    // fillet -- the long way is an arc that sweeps away round the outside.
    double sweep = to - from;
    while (sweep < 0.0) sweep += kTwoPi;
    if (sweep > kTwoPi / 2.0) std::swap(from, to);

    return Made({DrawLine{corner.firstFar, firstTouch}, DrawArc{centre, radiusMm, from, to},
                 DrawLine{secondTouch, corner.secondFar}});
}

SheetEditResult ChamferLines(const DrawShape& first, const DrawShape& second, Vec2 pickFirst,
                             Vec2 pickSecond, double setbackMm) {
    const auto* a = std::get_if<DrawLine>(&first);
    const auto* b = std::get_if<DrawLine>(&second);
    if (a == nullptr || b == nullptr) return Refuse("a chamfer joins two lines");
    if (!(setbackMm > 0.0)) return Refuse("a chamfer needs a setback bigger than nothing");

    const Corner corner = CornerOf(*a, *b, pickFirst, pickSecond);
    if (!corner.ok) return Refuse("these two lines are parallel, so they make no corner");

    const double firstReach = std::hypot(corner.firstFar.x - corner.at.x,
                                         corner.firstFar.y - corner.at.y);
    const double secondReach = std::hypot(corner.secondFar.x - corner.at.x,
                                          corner.secondFar.y - corner.at.y);
    if (setbackMm > firstReach + kTiny || setbackMm > secondReach + kTiny)
        return Refuse("a chamfer that deep is longer than the lines it would join");

    const Vec2 firstTouch{corner.at.x + corner.firstWay.x * setbackMm,
                          corner.at.y + corner.firstWay.y * setbackMm};
    const Vec2 secondTouch{corner.at.x + corner.secondWay.x * setbackMm,
                           corner.at.y + corner.secondWay.y * setbackMm};
    return Made({DrawLine{corner.firstFar, firstTouch}, DrawLine{firstTouch, secondTouch},
                 DrawLine{secondTouch, corner.secondFar}});
}

SheetEditResult OffsetShape(const DrawShape& shape, double distanceMm, Vec2 towards) {
    if (!(distanceMm > 0.0)) return Refuse("an offset distance has to be more than nothing");

    if (const auto* line = std::get_if<DrawLine>(&shape)) {
        const Vec2 along{line->b.x - line->a.x, line->b.y - line->a.y};
        const double length = std::hypot(along.x, along.y);
        if (length < kTiny) return Refuse("a line of no length has no side to offset to");
        // The normal, then turned to face the point the user picked. A signed
        // distance would work here and mean nothing for a circle, so the side
        // is said the same way for every shape: with a point.
        Vec2 normal{-along.y / length, along.x / length};
        const Vec2 middle{(line->a.x + line->b.x) / 2.0, (line->a.y + line->b.y) / 2.0};
        if ((towards.x - middle.x) * normal.x + (towards.y - middle.y) * normal.y < 0.0)
            normal = Vec2{-normal.x, -normal.y};
        return Made({DrawLine{Vec2{line->a.x + normal.x * distanceMm,
                                   line->a.y + normal.y * distanceMm},
                              Vec2{line->b.x + normal.x * distanceMm,
                                   line->b.y + normal.y * distanceMm}}});
    }

    if (const auto* circle = std::get_if<DrawCircle>(&shape)) {
        // OUTWARD IF THE PICK IS OUTSIDE, inward if it is in. Inward past the
        // centre is refused rather than mirrored into a bigger circle, which
        // is what a bare subtraction would silently produce.
        const double toPick = std::hypot(towards.x - circle->centre.x,
                                         towards.y - circle->centre.y);
        const double radius = toPick > circle->radius ? circle->radius + distanceMm
                                                      : circle->radius - distanceMm;
        if (!(radius > kTiny))
            return Refuse("an offset that far inward would turn this circle inside out");
        return Made({DrawCircle{circle->centre, radius}});
    }

    if (const auto* arc = std::get_if<DrawArc>(&shape)) {
        const double toPick = std::hypot(towards.x - arc->centre.x, towards.y - arc->centre.y);
        const double radius =
            toPick > arc->radius ? arc->radius + distanceMm : arc->radius - distanceMm;
        if (!(radius > kTiny))
            return Refuse("an offset that far inward would turn this arc inside out");
        // The ANGLES ARE UNCHANGED, which is what makes an offset arc
        // concentric: it is the same span of the same circle at a new radius.
        return Made({DrawArc{arc->centre, radius, arc->startAngle, arc->endAngle}});
    }

    return Refuse("this kind of object cannot be offset yet");
}

namespace {

DrawShape Moved(const DrawShape& shape, Vec2 by) {
    if (const auto* line = std::get_if<DrawLine>(&shape))
        return DrawLine{Vec2{line->a.x + by.x, line->a.y + by.y},
                        Vec2{line->b.x + by.x, line->b.y + by.y}};
    if (const auto* circle = std::get_if<DrawCircle>(&shape))
        return DrawCircle{Vec2{circle->centre.x + by.x, circle->centre.y + by.y},
                          circle->radius};
    if (const auto* arc = std::get_if<DrawArc>(&shape))
        return DrawArc{Vec2{arc->centre.x + by.x, arc->centre.y + by.y}, arc->radius,
                       arc->startAngle, arc->endAngle};
    if (const auto* point = std::get_if<DrawPoint>(&shape))
        return DrawPoint{Vec2{point->at.x + by.x, point->at.y + by.y}};
    if (const auto* text = std::get_if<DrawText>(&shape))
        return DrawText{Vec2{text->at.x + by.x, text->at.y + by.y}, text->text, text->heightMm,
                        text->rotation};
    if (const auto* ellipse = std::get_if<DrawEllipse>(&shape))
        return DrawEllipse{Vec2{ellipse->centre.x + by.x, ellipse->centre.y + by.y},
                           ellipse->majorRadius, ellipse->minorRadius, ellipse->rotation};
    if (const auto* poly = std::get_if<DrawPolyline>(&shape)) {
        DrawPolyline moved = *poly;
        for (DrawVertex& vertex : moved.vertices) {
            vertex.at.x += by.x;
            vertex.at.y += by.y;
        }
        return moved;
    }
    return shape;
}

Vec2 Turned(Vec2 point, Vec2 centre, double angle) {
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const double x = point.x - centre.x;
    const double y = point.y - centre.y;
    return Vec2{centre.x + x * c - y * s, centre.y + x * s + y * c};
}

DrawShape TurnedShape(const DrawShape& shape, Vec2 centre, double angle) {
    if (const auto* line = std::get_if<DrawLine>(&shape))
        return DrawLine{Turned(line->a, centre, angle), Turned(line->b, centre, angle)};
    if (const auto* circle = std::get_if<DrawCircle>(&shape))
        return DrawCircle{Turned(circle->centre, centre, angle), circle->radius};
    if (const auto* arc = std::get_if<DrawArc>(&shape))
        return DrawArc{Turned(arc->centre, centre, angle), arc->radius,
                       arc->startAngle + angle, arc->endAngle + angle};
    if (const auto* point = std::get_if<DrawPoint>(&shape))
        return DrawPoint{Turned(point->at, centre, angle)};
    if (const auto* text = std::get_if<DrawText>(&shape))
        return DrawText{Turned(text->at, centre, angle), text->text, text->heightMm,
                        text->rotation + angle};
    if (const auto* ellipse = std::get_if<DrawEllipse>(&shape))
        return DrawEllipse{Turned(ellipse->centre, centre, angle), ellipse->majorRadius,
                           ellipse->minorRadius, ellipse->rotation + angle};
    if (const auto* poly = std::get_if<DrawPolyline>(&shape)) {
        DrawPolyline turned = *poly;
        for (DrawVertex& vertex : turned.vertices) vertex.at = Turned(vertex.at, centre, angle);
        return turned;
    }
    return shape;
}

} // namespace

SheetEditResult RectangularArray(const std::vector<DrawShape>& shapes, int columns, int rows,
                                 Vec2 pitchMm) {
    if (shapes.empty()) return Refuse("nothing was chosen to array");
    if (columns < 1 || rows < 1)
        return Refuse("an array needs at least one row and one column");
    // A PITCH OF NOTHING STACKS EVERY COPY ON THE ORIGINAL. The drawing then
    // holds nine identical objects in one place, which looks exactly like one
    // object until somebody moves it.
    if (std::fabs(pitchMm.x) < kTiny && columns > 1)
        return Refuse("the columns are no distance apart, so they would all land on each other");
    if (std::fabs(pitchMm.y) < kTiny && rows > 1)
        return Refuse("the rows are no distance apart, so they would all land on each other");

    std::vector<DrawShape> out;
    out.reserve(shapes.size() * static_cast<std::size_t>(columns) *
                static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row)
        for (int column = 0; column < columns; ++column) {
            const Vec2 by{pitchMm.x * column, pitchMm.y * row};
            // THE ORIGINAL IS THE FIRST COPY, at an offset of nothing. Left
            // out, every caller has to add it back and the one that forgets
            // deletes it.
            for (const DrawShape& shape : shapes) out.push_back(Moved(shape, by));
        }
    return Made(std::move(out));
}

SheetEditResult PolarArray(const std::vector<DrawShape>& shapes, Vec2 centre, int count,
                           double totalAngleRad, bool rotateItems) {
    if (shapes.empty()) return Refuse("nothing was chosen to array");
    if (count < 1) return Refuse("a polar array needs at least one copy");
    if (std::fabs(totalAngleRad) < kTiny && count > 1)
        return Refuse("the copies span no angle, so they would all land on each other");

    // A FULL CIRCLE DOES NOT REPEAT ITS FIRST COPY AT THE END.
    //
    // Six bolts round a flange is six, not seven with two on top of each
    // other -- and two coincident objects look like one until somebody drags
    // it. So the step is the whole angle divided by the count when it closes,
    // and by count - 1 when it does not.
    const bool closes = std::fabs(std::fabs(totalAngleRad) - kTwoPi) < 1e-6;
    const double step = closes ? totalAngleRad / count
                               : (count > 1 ? totalAngleRad / (count - 1) : 0.0);

    std::vector<DrawShape> out;
    out.reserve(shapes.size() * static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const double angle = step * i;
        for (const DrawShape& shape : shapes) {
            if (rotateItems) {
                out.push_back(TurnedShape(shape, centre, angle));
            } else {
                // CARRIED ROUND WITHOUT TURNING: the position moves, the
                // object does not. This is what a ring of labels wants and
                // what a ring of bolts does not, which is why it is asked for
                // rather than assumed.
                // Where the shape's own anchor lands, applied as a MOVE.
                const Vec2 anchor = [&]() -> Vec2 {
                    if (const auto* line = std::get_if<DrawLine>(&shape)) return line->a;
                    if (const auto* circle = std::get_if<DrawCircle>(&shape))
                        return circle->centre;
                    if (const auto* arc = std::get_if<DrawArc>(&shape)) return arc->centre;
                    if (const auto* point = std::get_if<DrawPoint>(&shape)) return point->at;
                    if (const auto* text = std::get_if<DrawText>(&shape)) return text->at;
                    if (const auto* ellipse = std::get_if<DrawEllipse>(&shape))
                        return ellipse->centre;
                    if (const auto* poly = std::get_if<DrawPolyline>(&shape))
                        return poly->vertices.empty() ? Vec2{} : poly->vertices.front().at;
                    return Vec2{};
                }();
                const Vec2 landed = Turned(anchor, centre, angle);
                out.push_back(Moved(shape, Vec2{landed.x - anchor.x, landed.y - anchor.y}));
            }
        }
    }
    return Made(std::move(out));
}

} // namespace paramcad
