#include "Core/Drawing/DrawingEntity.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace paramcad {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kNoHit = std::numeric_limits<double>::max();
constexpr double kDefaultChordToleranceMm = 0.05;

Vec2 PointOnCircle(Vec2 centre, double radius, double angle) noexcept {
    return Vec2{centre.x + radius * std::cos(angle), centre.y + radius * std::sin(angle)};
}

// An ellipse, sampled. There is no closed form for the nearest point on one,
// and a drafting package does not need one: sampling finely enough to pick
// with is what every other package does too.
std::vector<Vec2> SampleEllipse(const DrawEllipse& ellipse, int samples) {
    std::vector<Vec2> points;
    points.reserve(static_cast<std::size_t>(samples) + 1);
    const double c = std::cos(ellipse.rotation);
    const double s = std::sin(ellipse.rotation);
    for (int i = 0; i <= samples; ++i) {
        const double t = kTwoPi * (static_cast<double>(i) / samples);
        const double x = ellipse.majorRadius * std::cos(t);
        const double y = ellipse.minorRadius * std::sin(t);
        points.push_back(Vec2{ellipse.centre.x + x * c - y * s,
                              ellipse.centre.y + x * s + y * c});
    }
    return points;
}

// A polyline's segments, each either a straight run or a bulged arc, visited
// once. THE one place a bulge is turned into geometry -- every operation that
// walks a polyline goes through here, so none of them can disagree about which
// way a bulge curves.
template <typename OnLine, typename OnArc>
void ForEachPolylineSegment(const DrawPolyline& polyline, OnLine onLine, OnArc onArc) {
    const std::size_t count = polyline.vertices.size();
    if (count < 2) return;
    const std::size_t last = polyline.closed ? count : count - 1;
    for (std::size_t i = 0; i < last; ++i) {
        const DrawVertex& from = polyline.vertices[i];
        const DrawVertex& to = polyline.vertices[(i + 1) % count];
        if (const std::optional<BulgeArc> arc = ArcFromBulge(from.at, to.at, from.bulge))
            onArc(*arc);
        else
            onLine(from.at, to.at);
    }
}

} // namespace

std::string_view ShapeName(const DrawShape& shape) noexcept {
    return std::visit(
        [](const auto& one) -> std::string_view {
            using T = std::decay_t<decltype(one)>;
            if constexpr (std::is_same_v<T, DrawPoint>) return "Point";
            else if constexpr (std::is_same_v<T, DrawLine>) return "Line";
            else if constexpr (std::is_same_v<T, DrawCircle>) return "Circle";
            else if constexpr (std::is_same_v<T, DrawArc>) return "Arc";
            else if constexpr (std::is_same_v<T, DrawEllipse>) return "Ellipse";
            else if constexpr (std::is_same_v<T, DrawPolyline>) return "Polyline";
            else return "Text";
        },
        shape);
}

// =============================================================================
// Bounds
// =============================================================================

Box2D BoundsOf(const DrawShape& shape) {
    Box2D box;
    if (const auto* point = std::get_if<DrawPoint>(&shape)) {
        box.grow(point->at);
    } else if (const auto* line = std::get_if<DrawLine>(&shape)) {
        box.grow(line->a);
        box.grow(line->b);
    } else if (const auto* circle = std::get_if<DrawCircle>(&shape)) {
        box.grow(Vec2{circle->centre.x - circle->radius, circle->centre.y - circle->radius});
        box.grow(Vec2{circle->centre.x + circle->radius, circle->centre.y + circle->radius});
    } else if (const auto* arc = std::get_if<DrawArc>(&shape)) {
        // THE ARC'S OWN EXTENT, not its circle's box. The same rule
        // ProjectedGeometry follows and for the same reason: taking the whole
        // circle makes every arc claim more room than it uses.
        box.grow(PointOnCircle(arc->centre, arc->radius, arc->startAngle));
        box.grow(PointOnCircle(arc->centre, arc->radius, arc->endAngle));
        for (int quarter = 0; quarter < 4; ++quarter) {
            const double cardinal = quarter * (kTwoPi / 4.0);
            if (AngleWithinArc(cardinal, arc->startAngle, arc->endAngle))
                box.grow(PointOnCircle(arc->centre, arc->radius, cardinal));
        }
    } else if (const auto* ellipse = std::get_if<DrawEllipse>(&shape)) {
        for (const Vec2 point : SampleEllipse(*ellipse, 64)) box.grow(point);
    } else if (const auto* polyline = std::get_if<DrawPolyline>(&shape)) {
        ForEachPolylineSegment(
            *polyline,
            [&](Vec2 a, Vec2 b) {
                box.grow(a);
                box.grow(b);
            },
            [&](const BulgeArc& arc) {
                for (const Vec2 point : TessellateArc(arc.centre, arc.radius, arc.startAngle,
                                                      arc.endAngle, kDefaultChordToleranceMm))
                    box.grow(point);
            });
        // A single vertex still occupies a place.
        if (polyline->vertices.size() == 1) box.grow(polyline->vertices.front().at);
    } else if (const auto* text = std::get_if<DrawText>(&shape)) {
        // AN ESTIMATE, AND SAID SO. The real width needs a font, which is a
        // presentation concern and not in Core. 0.6 of the height per
        // character is the usual figure for an ISO stroke font, and it is used
        // for PICKING and EXTENT only -- never for placing anything, where
        // being wrong would show.
        const double width = 0.6 * text->heightMm * static_cast<double>(text->text.size());
        const double c = std::cos(text->rotation);
        const double s = std::sin(text->rotation);
        box.grow(text->at);
        box.grow(Vec2{text->at.x + width * c, text->at.y + width * s});
        box.grow(Vec2{text->at.x - text->heightMm * s, text->at.y + text->heightMm * c});
        box.grow(Vec2{text->at.x + width * c - text->heightMm * s,
                      text->at.y + width * s + text->heightMm * c});
    }
    return box;
}

// =============================================================================
// Picking
// =============================================================================

double DistanceFrom(const DrawShape& shape, Vec2 point) {
    if (const auto* one = std::get_if<DrawPoint>(&shape))
        return std::hypot(point.x - one->at.x, point.y - one->at.y);
    if (const auto* line = std::get_if<DrawLine>(&shape))
        return DistancePointToSegment(point, line->a, line->b);
    if (const auto* circle = std::get_if<DrawCircle>(&shape)) {
        // THE RIM, NOT THE DISC. A circle is a curve; clicking its middle
        // selects whatever is under the middle, which on a drawing is usually
        // the thing the circle is a hole in.
        const double toCentre = std::hypot(point.x - circle->centre.x, point.y - circle->centre.y);
        return std::fabs(toCentre - circle->radius);
    }
    if (const auto* arc = std::get_if<DrawArc>(&shape)) {
        const double angle = std::atan2(point.y - arc->centre.y, point.x - arc->centre.x);
        if (AngleWithinArc(angle, arc->startAngle, arc->endAngle)) {
            const double toCentre = std::hypot(point.x - arc->centre.x, point.y - arc->centre.y);
            return std::fabs(toCentre - arc->radius);
        }
        // OFF THE SWEEP: the nearest END, not the nearest point on the whole
        // circle. Measuring to the circle would let a click on the far side of
        // a quarter arc select it, which is the arc equivalent of picking a
        // line by its infinite extension.
        const Vec2 start = PointOnCircle(arc->centre, arc->radius, arc->startAngle);
        const Vec2 end = PointOnCircle(arc->centre, arc->radius, arc->endAngle);
        return std::min(std::hypot(point.x - start.x, point.y - start.y),
                        std::hypot(point.x - end.x, point.y - end.y));
    }
    if (const auto* ellipse = std::get_if<DrawEllipse>(&shape)) {
        double best = kNoHit;
        const std::vector<Vec2> samples = SampleEllipse(*ellipse, 128);
        for (std::size_t i = 0; i + 1 < samples.size(); ++i)
            best = std::min(best, DistancePointToSegment(point, samples[i], samples[i + 1]));
        return best;
    }
    if (const auto* polyline = std::get_if<DrawPolyline>(&shape)) {
        double best = kNoHit;
        ForEachPolylineSegment(
            *polyline,
            [&](Vec2 a, Vec2 b) { best = std::min(best, DistancePointToSegment(point, a, b)); },
            [&](const BulgeArc& arc) {
                DrawArc as;
                as.centre = arc.centre;
                as.radius = arc.radius;
                as.startAngle = arc.startAngle;
                as.endAngle = arc.endAngle;
                best = std::min(best, DistanceFrom(DrawShape{as}, point));
            });
        if (polyline->vertices.size() == 1)
            best = std::min(best, std::hypot(point.x - polyline->vertices.front().at.x,
                                             point.y - polyline->vertices.front().at.y));
        return best;
    }
    if (std::holds_alternative<DrawText>(shape)) {
        // TEXT IS PICKED BY ITS BOX, which is what every drafting package
        // does: a stroke-accurate hit test on a glyph would make single
        // characters nearly unselectable.
        const Box2D box = BoundsOf(shape);
        if (box.contains(point)) return 0.0;
        const Vec2 near{std::clamp(point.x, box.min.x, box.max.x),
                        std::clamp(point.y, box.min.y, box.max.y)};
        return std::hypot(point.x - near.x, point.y - near.y);
    }
    return kNoHit;
}

Vec2 ClosestPointOn(const DrawShape& shape, Vec2 point) {
    if (const auto* one = std::get_if<DrawPoint>(&shape)) return one->at;
    if (const auto* line = std::get_if<DrawLine>(&shape))
        return ClosestPointOnSegment(point, line->a, line->b);
    if (const auto* circle = std::get_if<DrawCircle>(&shape)) {
        const double dx = point.x - circle->centre.x;
        const double dy = point.y - circle->centre.y;
        const double length = std::hypot(dx, dy);
        if (!(length > 1.0e-12)) return PointOnCircle(circle->centre, circle->radius, 0.0);
        return Vec2{circle->centre.x + circle->radius * dx / length,
                    circle->centre.y + circle->radius * dy / length};
    }
    if (const auto* arc = std::get_if<DrawArc>(&shape)) {
        const double angle = std::atan2(point.y - arc->centre.y, point.x - arc->centre.x);
        if (AngleWithinArc(angle, arc->startAngle, arc->endAngle))
            return PointOnCircle(arc->centre, arc->radius, angle);
        const Vec2 start = PointOnCircle(arc->centre, arc->radius, arc->startAngle);
        const Vec2 end = PointOnCircle(arc->centre, arc->radius, arc->endAngle);
        return std::hypot(point.x - start.x, point.y - start.y) <=
                       std::hypot(point.x - end.x, point.y - end.y)
                   ? start
                   : end;
    }
    // Everything else, off its own flattening -- which is what the sampling
    // in DistanceFrom already amounts to, done once here.
    const std::vector<Vec2> points = FlattenShape(shape, kDefaultChordToleranceMm);
    Vec2 best = point;
    double bestDistance = kNoHit;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const Vec2 near = ClosestPointOnSegment(point, points[i], points[i + 1]);
        const double distance = std::hypot(point.x - near.x, point.y - near.y);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = near;
        }
    }
    if (points.size() == 1) return points.front();
    return best;
}

// =============================================================================
// Flattening
// =============================================================================

std::vector<Vec2> FlattenShape(const DrawShape& shape, double chordToleranceMm) {
    std::vector<Vec2> points;
    if (const auto* one = std::get_if<DrawPoint>(&shape)) {
        points.push_back(one->at);
    } else if (const auto* line = std::get_if<DrawLine>(&shape)) {
        points.push_back(line->a);
        points.push_back(line->b);
    } else if (const auto* circle = std::get_if<DrawCircle>(&shape)) {
        points = TessellateArc(circle->centre, circle->radius, 0.0, kTwoPi, chordToleranceMm);
    } else if (const auto* arc = std::get_if<DrawArc>(&shape)) {
        points = TessellateArc(arc->centre, arc->radius, arc->startAngle, arc->endAngle,
                               chordToleranceMm);
    } else if (const auto* ellipse = std::get_if<DrawEllipse>(&shape)) {
        // SAMPLE COUNT FROM THE TOLERANCE, using the MAJOR radius -- the
        // tightest part of an ellipse is where it curves most, and that is
        // governed by the long axis.
        const double radius = std::max(ellipse->majorRadius, ellipse->minorRadius);
        const double tolerance = std::max(1.0e-6, chordToleranceMm);
        int samples = 32;
        if (tolerance < radius)
            samples = std::max(
                8, static_cast<int>(std::ceil(kTwoPi / (2.0 * std::acos(1.0 - tolerance / radius)))));
        points = SampleEllipse(*ellipse, samples);
    } else if (const auto* polyline = std::get_if<DrawPolyline>(&shape)) {
        ForEachPolylineSegment(
            *polyline,
            [&](Vec2 a, Vec2 b) {
                if (points.empty()) points.push_back(a);
                points.push_back(b);
            },
            [&](const BulgeArc& arc) {
                std::vector<Vec2> along = TessellateArc(
                    arc.centre, arc.radius, arc.startAngle, arc.endAngle, chordToleranceMm);
                // IN TRAVEL ORDER, not in sweep order.
                //
                // An arc's angles are always stored counter-clockwise (one
                // convention, so nothing downstream has to ask), and
                // TessellateArc walks them that way. A polyline segment
                // travelled CLOCKWISE therefore comes back end-first -- which
                // is a polyline whose points run backwards through every arc,
                // and whose ends are each other's.
                //
                // `counterClockwise` exists for exactly this and was read by
                // nothing until M33_GEO_006 measured which end came out first.
                if (!arc.counterClockwise) std::reverse(along.begin(), along.end());
                for (std::size_t i = 0; i < along.size(); ++i) {
                    if (i == 0 && !points.empty()) continue; // the joint, once
                    points.push_back(along[i]);
                }
            });
        if (polyline->vertices.size() == 1) points.push_back(polyline->vertices.front().at);
    } else if (const auto* text = std::get_if<DrawText>(&shape)) {
        // TEXT DOES NOT FLATTEN TO A CURVE. Its box is the honest answer for
        // an extent, and a caller wanting outlines needs a font, which Core
        // has not got.
        const Box2D box = BoundsOf(DrawShape{*text});
        if (!box.empty) {
            points.push_back(box.min);
            points.push_back(Vec2{box.max.x, box.min.y});
            points.push_back(box.max);
            points.push_back(Vec2{box.min.x, box.max.y});
            points.push_back(box.min);
        }
    }
    return points;
}

// =============================================================================
// Transforms
// =============================================================================

DrawShape TransformShape(const DrawShape& shape, const Matrix2D& transform) {
    if (const auto* one = std::get_if<DrawPoint>(&shape))
        return DrawPoint{transform.apply(one->at)};
    if (const auto* line = std::get_if<DrawLine>(&shape))
        return DrawLine{transform.apply(line->a), transform.apply(line->b)};

    if (const auto* circle = std::get_if<DrawCircle>(&shape)) {
        // A CIRCLE UNDER A NON-UNIFORM TRANSFORM IS AN ELLIPSE, and saying so
        // is the point. Scaling the radius by an average would draw a circle
        // where the model has an oval, and every measurement taken off it
        // afterwards would be wrong by a different amount in each direction.
        if (!transform.isUniform()) {
            DrawEllipse out;
            out.centre = transform.apply(circle->centre);
            const Vec2 major = transform.applyDirection(Vec2{circle->radius, 0.0});
            const Vec2 minor = transform.applyDirection(Vec2{0.0, circle->radius});
            out.majorRadius = std::hypot(major.x, major.y);
            out.minorRadius = std::hypot(minor.x, minor.y);
            out.rotation = std::atan2(major.y, major.x);
            if (out.minorRadius > out.majorRadius) {
                std::swap(out.majorRadius, out.minorRadius);
                out.rotation = std::atan2(minor.y, minor.x);
            }
            return out;
        }
        return DrawCircle{transform.apply(circle->centre),
                          circle->radius * transform.scaleFactor()};
    }

    if (const auto* arc = std::get_if<DrawArc>(&shape)) {
        // THE ENDS ARE MOVED AND THE ANGLES RE-MEASURED, rather than the
        // angles being rotated by the transform's own rotation. That is what
        // makes a MIRROR work: reflection reverses the sweep, so the arc that
        // comes back runs from what was the end to what was the start -- and
        // an implementation that added a rotation to both angles would mirror
        // the ends and keep the bulge on the wrong side.
        const Vec2 centre = transform.apply(arc->centre);
        const Vec2 start = transform.apply(PointOnCircle(arc->centre, arc->radius,
                                                         arc->startAngle));
        const Vec2 end = transform.apply(PointOnCircle(arc->centre, arc->radius, arc->endAngle));
        DrawArc out;
        out.centre = centre;
        out.radius = arc->radius * transform.scaleFactor();
        const double startAngle = std::atan2(start.y - centre.y, start.x - centre.x);
        const double endAngle = std::atan2(end.y - centre.y, end.x - centre.x);
        // Does this transform flip orientation? The determinant says so, and
        // a flip swaps which end the counter-clockwise sweep begins at.
        const double determinant =
            transform.m00 * transform.m11 - transform.m01 * transform.m10;
        if (determinant < 0.0) {
            out.startAngle = NormaliseAngle(endAngle);
            out.endAngle = NormaliseAngle(startAngle);
        } else {
            out.startAngle = NormaliseAngle(startAngle);
            out.endAngle = NormaliseAngle(endAngle);
        }
        return out;
    }

    if (const auto* ellipse = std::get_if<DrawEllipse>(&shape)) {
        DrawEllipse out;
        out.centre = transform.apply(ellipse->centre);
        const double c = std::cos(ellipse->rotation);
        const double s = std::sin(ellipse->rotation);
        const Vec2 major = transform.applyDirection(
            Vec2{ellipse->majorRadius * c, ellipse->majorRadius * s});
        const Vec2 minor = transform.applyDirection(
            Vec2{-ellipse->minorRadius * s, ellipse->minorRadius * c});
        out.majorRadius = std::hypot(major.x, major.y);
        out.minorRadius = std::hypot(minor.x, minor.y);
        out.rotation = std::atan2(major.y, major.x);
        if (out.minorRadius > out.majorRadius) {
            std::swap(out.majorRadius, out.minorRadius);
            out.rotation = std::atan2(minor.y, minor.x);
        }
        return out;
    }

    if (const auto* polyline = std::get_if<DrawPolyline>(&shape)) {
        DrawPolyline out;
        out.closed = polyline->closed;
        const double determinant =
            transform.m00 * transform.m11 - transform.m01 * transform.m10;
        const bool flipped = determinant < 0.0;
        // A MIRRORED BULGE CHANGES SIGN, and NOTHING ELSE CHANGES.
        //
        // The bulge encodes which side of the chord the arc lies on, so a
        // reflection puts it on the other -- moving the vertices alone would
        // leave every arc in a mirrored polyline bulging back the way it came.
        //
        // THE VERTEX ORDER IS NOT REVERSED. The first draft reversed it as
        // well, reasoning that a bulge lives on the vertex its segment starts
        // at -- true, but the traversal order is unchanged by a mirror, so
        // reversing it undid the sign flip exactly and the arc came out flat.
        // Two corrections that cancel are worse than neither, because the
        // result looks like the transform simply not working.
        for (const DrawVertex& vertex : polyline->vertices)
            out.vertices.push_back(
                DrawVertex{transform.apply(vertex.at), flipped ? -vertex.bulge : vertex.bulge});
        return out;
    }

    if (const auto* text = std::get_if<DrawText>(&shape)) {
        DrawText out = *text;
        out.at = transform.apply(text->at);
        const Vec2 along = transform.applyDirection(
            Vec2{std::cos(text->rotation), std::sin(text->rotation)});
        out.rotation = std::atan2(along.y, along.x);
        out.heightMm = text->heightMm * transform.scaleFactor();
        return out;
    }
    return shape;
}

// =============================================================================
// DrawingEntity
// =============================================================================

DrawingEntity::DrawingEntity(DrawShape shape, ObjectId layerId)
    : id_(ObjectIdGenerator::Next()), shape_(std::move(shape)), layerId_(layerId) {}

DrawingEntity::DrawingEntity(ObjectId id, DrawShape shape, ObjectId layerId, int color,
                             std::string linetype, int lineweight)
    : id_(RestoreObjectId(id)),
      shape_(std::move(shape)),
      layerId_(layerId),
      color_(color),
      linetype_(std::move(linetype)),
      lineweight_(lineweight) {}

Box2D DrawingEntity::bounds() const { return BoundsOf(shape_); }
double DrawingEntity::distanceTo(Vec2 point) const { return DistanceFrom(shape_, point); }
Vec2 DrawingEntity::closestPointTo(Vec2 point) const { return ClosestPointOn(shape_, point); }
std::vector<Vec2> DrawingEntity::flatten(double chordToleranceMm) const {
    return FlattenShape(shape_, chordToleranceMm);
}
void DrawingEntity::applyTransform(const Matrix2D& transform) {
    shape_ = TransformShape(shape_, transform);
}

} // namespace paramcad
