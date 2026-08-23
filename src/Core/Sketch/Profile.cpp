#include "Core/Sketch/Profile.h"
#include <variant>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <map>
#include <vector>

namespace paramcad {

namespace {

ProfileResult Failure(ProfileError error, std::string message) {
    return ProfileResult{ValidatedProfile{}, error, std::move(message)};
}

std::string IdText(SketchEntityId id) { return std::to_string(ToObjectId(id)); }

// Curves that can take part in a chained loop, in ascending SketchEntityId
// order. Sorting by id -- not by storage position -- is what makes traversal
// independent of the order entities were added, removed or restored in
// (ADR-M4-005).
struct Curve {
    SketchEntityId id{kInvalidSketchEntityId};
    Vec2 start{};
    Vec2 end{};
    bool used{false};
};

// One geometric location, shared by every endpoint within tolerance of it.
struct Junction {
    Vec2 position{};
    int incidence{0};
};

std::size_t JunctionIndexFor(std::vector<Junction>& junctions, Vec2 point) {
    for (std::size_t i = 0; i < junctions.size(); ++i)
        if (SamePoint(junctions[i].position, point, kProfileConnectivityToleranceMm)) return i;
    junctions.push_back(Junction{point, 0});
    return junctions.size() - 1;
}

// Proper segment intersection test for the self-intersection check. Returns
// true only when the two segments genuinely cross or overlap, not when they
// merely share an endpoint (adjacent loop members always do).
bool SegmentsProperlyIntersect(Vec2 p1, Vec2 p2, Vec2 q1, Vec2 q2) noexcept {
    const auto cross = [](Vec2 o, Vec2 a, Vec2 b) noexcept {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };
    const double d1 = cross(q1, q2, p1);
    const double d2 = cross(q1, q2, p2);
    const double d3 = cross(p1, p2, q1);
    const double d4 = cross(p1, p2, q2);

    // Strict sign change on both segments: a genuine crossing.
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
        ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0)))
        return true;

    // Collinear overlap: an endpoint of one lying strictly inside the other.
    //
    // Parametric, NOT bounding-box. An earlier bounding-box form shrank the x
    // bounds while growing the y bounds, so for any segment narrower than the
    // tolerance in x -- every vertical segment -- the test could never fire,
    // and an outline whose vertical members overlapped was accepted as valid.
    // OCCT then built a degenerate face and reported a volume of zero as a
    // successful result: silent wrong geometry, caught by independent review.
    //
    // Projecting onto the segment instead is axis-independent by construction,
    // which is what makes the whole class of orientation-dependent bugs
    // impossible rather than merely fixed in the case that was found.
    const auto onSegment = [](Vec2 a, Vec2 b, Vec2 p) noexcept {
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        const double lengthSquared = dx * dx + dy * dy;
        if (lengthSquared <= 0.0) return false;
        const double length = std::sqrt(lengthSquared);

        // Perpendicular distance, normalized: the raw cross product scales with
        // segment length, so comparing it to a length tolerance directly would
        // make the effective tolerance depend on how long the segment is.
        const double cross = dx * (p.y - a.y) - dy * (p.x - a.x);
        if (std::fabs(cross) / length > kProfileConnectivityToleranceMm) return false;

        // Position along the segment, in (0, 1). "Strictly inside" excludes a
        // shared endpoint, which adjacent loop members legitimately have.
        const double t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSquared;
        const double margin = kProfileConnectivityToleranceMm / length;
        return t > margin && t < 1.0 - margin;
    };
    return onSegment(p1, p2, q1) || onSegment(p1, p2, q2) || onSegment(q1, q2, p1) ||
           onSegment(q1, q2, p2);
}

// ONE translator for every chain. A profile's outer boundary, one of its
// holes, and a sweep's spine differ in what they MEAN, not in how a curve
// becomes a segment -- and a second copy of the arc-reversal rule (or, since
// M18, the spline-handle-negation rule) would be a second place to disagree
// about it, which is a geometry bug that reads as a solver bug.
//
// Promoted out of BuildKernelProfile when M19 needed the same translation for a
// path. Copying it would have been the smaller edit and the larger mistake.
bool TranslateChain(const Sketch& sketch, const ProfileLoop& loop,
                    std::vector<ProfileSegment>& segments) {
    segments.reserve(loop.entities.size());
    for (const OrientedSketchEntityRef& ref : loop.entities) {
        const SketchEntity* entity = sketch.findEntity(ref.entityId);
        if (entity == nullptr) return false;

        if (const auto* line = std::get_if<SketchLine>(&entity->geometry)) {
            segments.push_back(ref.reversed
                                   ? ProfileLineSegment{line->end, line->start}
                                   : ProfileLineSegment{line->start, line->end});
        } else if (const auto* arc = std::get_if<SketchArc>(&entity->geometry)) {
            // Reversing an arc swaps its endpoints AND its direction, so
            // the traversal still runs start-to-end along the same curve.
            segments.push_back(
                ref.reversed
                    ? ProfileArcSegment{arc->center, arc->radiusMm, arc->endAngleRad,
                                        arc->startAngleRad, !arc->counterClockwise}
                    : ProfileArcSegment{arc->center, arc->radiusMm, arc->startAngleRad,
                                        arc->endAngleRad, arc->counterClockwise});
        } else if (const auto* piece =
                       std::get_if<SketchEllipticalArc>(&entity->geometry)) {
            // Reversed the same way an arc is: swap the two parameters AND
            // the direction, so the traversal still runs start-to-end along
            // the same curve.
            segments.push_back(
                ref.reversed
                    ? ProfileEllipticalArcSegment{
                          piece->center, piece->majorRadiusMm, piece->minorRadiusMm,
                          piece->rotationRad, piece->endParamRad, piece->startParamRad,
                          !piece->counterClockwise}
                    : ProfileEllipticalArcSegment{
                          piece->center, piece->majorRadiusMm, piece->minorRadiusMm,
                          piece->rotationRad, piece->startParamRad, piece->endParamRad,
                          piece->counterClockwise});
        } else if (const auto* circle = std::get_if<SketchCircle>(&entity->geometry)) {
            segments.push_back(ProfileCircleSegment{circle->center, circle->radiusMm});
        } else if (const auto* full = std::get_if<SketchEllipse>(&entity->geometry)) {
            segments.push_back(ProfileEllipseSegment{full->center, full->majorRadiusMm,
                                                     full->minorRadiusMm,
                                                     full->rotationRad});
        } else if (const auto* spline = std::get_if<SketchSpline>(&entity->geometry)) {
            // REVERSED BY REVERSING THE POINTS, which is all a spline's
            // direction is -- there is no sweep flag to flip and no angles
            // to swap. The curve through a reversed list is the same curve
            // traversed the other way.
            std::vector<Vec2> points = spline->points;
            std::map<int, Vec2> handles = spline->handles;
            if (ref.reversed) {
                std::reverse(points.begin(), points.end());
                // A HANDLE REVERSES TWICE: point i becomes point n-1-i, and
                // its tangent turns round, because the direction the curve
                // LEAVES a point going one way is the direction it ARRIVES
                // going the other. Renumbering without negating would give
                // the reversed profile a curve that bulges the wrong way at
                // every handled point -- the same shape flipped, which is
                // not the same shape.
                const int last = static_cast<int>(points.size()) - 1;
                std::map<int, Vec2> flipped;
                for (const auto& [index, tangent] : handles)
                    flipped.emplace(last - index, Vec2{-tangent.x, -tangent.y});
                handles = std::move(flipped);
            }
            segments.push_back(
                ProfileSplineSegment{std::move(points), spline->closed, std::move(handles)});
        } else {
            return false; // a Point can never be a loop member
        }
    }
    return true;
}

} // namespace

ProfileResult BuildProfile(const Sketch& sketch) {
    return BuildProfile(sketch, kInvalidSketchEntityId);
}

// A loop as a closed polygon, in loop order, with curves tessellated.
//
// Used ONLY to decide containment. The tessellation is deliberately coarse: the
// question is "is this loop inside that one", and loops that are so close
// together that the answer depends on chord accuracy are loops OCCT will refuse
// anyway. Nothing built from this reaches the kernel -- the kernel gets the
// exact curves.
std::vector<Vec2> LoopPolygonImpl(const Sketch& sketch, const ProfileLoop& loop, bool closed) {
    constexpr int kArcSteps = 24;
    std::vector<Vec2> polygon;
    for (const OrientedSketchEntityRef& ref : loop.entities) {
        const SketchEntity* entity = sketch.findEntity(ref.entityId);
        if (entity == nullptr) continue;
        if (const auto* line = std::get_if<SketchLine>(&entity->geometry)) {
            polygon.push_back(ref.reversed ? line->end : line->start);
        } else if (const auto* arc = std::get_if<SketchArc>(&entity->geometry)) {
            double from = ref.reversed ? arc->endAngleRad : arc->startAngleRad;
            double to = ref.reversed ? arc->startAngleRad : arc->endAngleRad;
            const bool ccw = ref.reversed ? !arc->counterClockwise : arc->counterClockwise;
            double sweep = to - from;
            const double twoPi = 6.283185307179586476925286766559;
            while (ccw && sweep < 0.0) sweep += twoPi;
            while (!ccw && sweep > 0.0) sweep -= twoPi;
            for (int step = 0; step < kArcSteps; ++step) {
                const double angle = from + sweep * (static_cast<double>(step) / kArcSteps);
                polygon.push_back(Vec2{arc->center.x + arc->radiusMm * std::cos(angle),
                                       arc->center.y + arc->radiusMm * std::sin(angle)});
            }
        } else if (const auto* piece = std::get_if<SketchEllipticalArc>(&entity->geometry)) {
            double from = ref.reversed ? piece->endParamRad : piece->startParamRad;
            const double to = ref.reversed ? piece->startParamRad : piece->endParamRad;
            const bool ccw = ref.reversed ? !piece->counterClockwise : piece->counterClockwise;
            double sweep = to - from;
            const double twoPi = 6.283185307179586476925286766559;
            while (ccw && sweep < 0.0) sweep += twoPi;
            while (!ccw && sweep > 0.0) sweep -= twoPi;
            for (int step = 0; step < kArcSteps; ++step)
                polygon.push_back(PointOnEllipse(
                    piece->center, piece->majorRadiusMm, piece->minorRadiusMm,
                    piece->rotationRad, from + sweep * (static_cast<double>(step) / kArcSteps)));
        } else if (const auto* circle = std::get_if<SketchCircle>(&entity->geometry)) {
            for (int step = 0; step < kArcSteps * 2; ++step) {
                const double angle =
                    6.283185307179586476925286766559 * step / (kArcSteps * 2);
                polygon.push_back(Vec2{circle->center.x + circle->radiusMm * std::cos(angle),
                                       circle->center.y + circle->radiusMm * std::sin(angle)});
            }
        } else if (const auto* full = std::get_if<SketchEllipse>(&entity->geometry)) {
            for (int step = 0; step < kArcSteps * 2; ++step)
                polygon.push_back(PointOnEllipse(
                    full->center, full->majorRadiusMm, full->minorRadiusMm, full->rotationRad,
                    6.283185307179586476925286766559 * step / (kArcSteps * 2)));
        } else if (const auto* spline = std::get_if<SketchSpline>(&entity->geometry)) {
            // THROUGH THE SHARED SAMPLER, so the polygon this winding test uses
            // is the same curve the canvas draws. A second sampling rule here
            // would let the two disagree about which loop is inside which.
            std::vector<Vec2> sampled = SampleSpline(*spline, 8);
            if (ref.reversed) std::reverse(sampled.begin(), sampled.end());
            // The LAST point is dropped: it is the next entity's first, and a
            // polygon that repeated every joint would have zero-length edges.
            if (!sampled.empty() && !spline->closed) sampled.pop_back();
            polygon.insert(polygon.end(), sampled.begin(), sampled.end());
        }
    }
    // THE FINAL POINT, for an OPEN chain only.
    //
    // Each branch above contributes a piece's START and leaves its END to the
    // next piece -- which is right for a ring, where the last piece's end IS
    // the first piece's start, and wrong for a path, where dropping it would
    // lose the last segment entirely. A curve pattern spacing copies along
    // such a polyline would then stop short of the end.
    if (!closed && !loop.entities.empty()) {
        const OrientedSketchEntityRef& last = loop.entities.back();
        const SketchEntity* entity = sketch.findEntity(last.entityId);
        if (entity != nullptr && HasEndpoints(entity->geometry))
            polygon.push_back(last.reversed ? StartPointOf(entity->geometry)
                                            : EndPointOf(entity->geometry));
    }
    return polygon;
}

std::vector<Vec2> LoopPolygon(const Sketch& sketch, const ProfileLoop& loop) {
    return LoopPolygonImpl(sketch, loop, true);
}

// Ray casting: is `p` inside `polygon`?
bool PointInPolygon(const std::vector<Vec2>& polygon, Vec2 p) noexcept {
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const Vec2& a = polygon[i];
        const Vec2& b = polygon[j];
        if ((a.y > p.y) == (b.y > p.y)) continue;
        const double x = (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x;
        if (p.x < x) inside = !inside;
    }
    return inside;
}

// Is EVERY vertex of `inner` inside `outer`? `crosses` is set when some are and
// some are not -- which means the two boundaries cross, and neither contains
// the other.
//
// Every vertex rather than one: a single sample cannot tell a contained loop
// from one that merely starts inside, and "mostly inside" is not a thing a
// profile can be.
bool PolygonInsidePolygon(const std::vector<Vec2>& inner, const std::vector<Vec2>& outer,
                          bool& crosses) noexcept {
    crosses = false;
    if (inner.empty() || outer.size() < 3) return false;
    std::size_t insideCount = 0;
    for (const Vec2& vertex : inner)
        if (PointInPolygon(outer, vertex)) ++insideCount;
    if (insideCount == inner.size()) return true;
    if (insideCount != 0) crosses = true;
    return false;
}

ProfileResult BuildProfile(const Sketch& sketch, SketchEntityId excludedEntityId) {
    // 1. Partition entities. Points are reference geometry: they cannot
    //    contribute an edge, so they are ignored rather than rejected. The
    //    excluded entity (a revolve axis) is construction geometry for the
    //    same reason -- present in the drawing, contributing no edge.
    std::vector<Curve> curves;
    std::vector<SketchEntityId> closedCurves; // full circles
    for (const SketchEntity& entity : sketch.entities()) {
        if (entity.id == excludedEntityId) continue;
        if (std::holds_alternative<SketchPoint>(entity.geometry)) continue;
        // Construction geometry contributes no edge, for the same reason a
        // Point does not: it is in the drawing to be measured from, not to be
        // swept. Ignored rather than rejected -- a sketch is not broken for
        // containing a centreline.
        if (entity.construction) continue;

        // Re-checked here even though Sketch::add* already rejects invalid
        // geometry: a restored entity can carry bad data from a hand-edited
        // document that never went through add* (defense in depth, the same
        // pattern MassPropertiesNode uses for its upstream check).
        if (!IsValidSketchGeometry(entity.geometry))
            return Failure(ProfileError::InvalidGeometry,
                           "entity " + IdText(entity.id) +
                               " has invalid geometry (non-finite, degenerate, or "
                               "non-positive radius)");

        if (!HasEndpoints(entity.geometry)) {
            closedCurves.push_back(entity.id);
            continue;
        }
        curves.push_back(Curve{entity.id, StartPointOf(entity.geometry),
                               EndPointOf(entity.geometry), false});
    }

    // 2. A full circle is a complete loop by itself and cannot chain with
    //    anything, so any other curve alongside it is ambiguous.
    // Closed curves are NOT rejected here any more (M17). Each becomes a loop
    // of its own in step 6, and step 8 works out which loop contains which --
    // a circle inside a rectangle is the commonest hole in mechanical CAD, and
    // refusing the combination outright refused the commonest profile there is.
    if (curves.empty() && closedCurves.empty())
        return Failure(ProfileError::NoEntities, "sketch contains no curves to form a profile");

    std::sort(curves.begin(), curves.end(),
              [](const Curve& a, const Curve& b) { return ToObjectId(a.id) < ToObjectId(b.id); });

    // 3. Duplicate detection: two entities describing the SAME curve.
    //
    //    Sharing endpoints is deliberately not the test. An arc and its chord
    //    legitimately span the same two points (a half-disc is a valid
    //    two-entity loop), as do the two arcs of a lens. Comparing whole curves
    //    -- kind, span and midpoint -- rejects real duplicates without
    //    rejecting those.
    for (std::size_t i = 0; i < curves.size(); ++i) {
        for (std::size_t j = i + 1; j < curves.size(); ++j) {
            const SketchEntity* first = sketch.findEntity(curves[i].id);
            const SketchEntity* second = sketch.findEntity(curves[j].id);
            if (first == nullptr || second == nullptr) continue;
            if (!IsSameCurve(first->geometry, second->geometry,
                             kProfileConnectivityToleranceMm))
                continue;
            return Failure(ProfileError::DuplicateEntity,
                           "entities " + IdText(curves[i].id) + " and " +
                               IdText(curves[j].id) + " describe the same curve");
        }
    }

    // 4. Incidence: in a single closed loop every junction joins exactly two
    //    curve ends. One means the loop is open there; three or more is a
    //    branch, which is ambiguous and must be rejected, never guessed.
    std::vector<Junction> junctions;
    for (const Curve& curve : curves) {
        ++junctions[JunctionIndexFor(junctions, curve.start)].incidence;
        ++junctions[JunctionIndexFor(junctions, curve.end)].incidence;
    }
    for (const Junction& junction : junctions) {
        if (junction.incidence == 2) continue;
        if (junction.incidence < 2)
            return Failure(ProfileError::OpenLoop,
                           "profile is not closed: an endpoint near (" +
                               std::to_string(junction.position.x) + ", " +
                               std::to_string(junction.position.y) +
                               ") is used by only one curve");
        return Failure(ProfileError::Branch,
                       "profile branches: an endpoint near (" +
                           std::to_string(junction.position.x) + ", " +
                           std::to_string(junction.position.y) + ") is used by " +
                           std::to_string(junction.incidence) + " curve ends");
    }

    // 5. Walk EVERY component. Each closed walk is a loop; a rectangle with a
    //    circle inside it is two, which is what a hole is made of (M17).
    //
    //    Incidence above already proved every junction has degree two, so a
    //    walk that cannot advance has run out of curves in ITS component --
    //    which is now an ordinary outcome, not the failure it used to be.
    std::vector<ProfileLoop> loops;
    std::size_t remaining = curves.size();
    while (remaining > 0) {
        Curve* seed = nullptr;
        for (Curve& candidate : curves)
            if (!candidate.used) {
                seed = &candidate;
                break;
            }
        if (seed == nullptr) break;

        ProfileLoop loop;
        seed->used = true;
        --remaining;
        loop.entities.push_back(OrientedSketchEntityRef{seed->id, false});
        const Vec2 loopStart = seed->start;
        Vec2 cursor = seed->end;

        while (!SamePoint(cursor, loopStart, kProfileConnectivityToleranceMm)) {
            bool advanced = false;
            for (Curve& candidate : curves) {
                if (candidate.used) continue;
                const bool forward =
                    SamePoint(candidate.start, cursor, kProfileConnectivityToleranceMm);
                const bool backward =
                    SamePoint(candidate.end, cursor, kProfileConnectivityToleranceMm);
                if (!forward && !backward) continue;
                candidate.used = true;
                --remaining;
                loop.entities.push_back(OrientedSketchEntityRef{candidate.id, backward});
                cursor = forward ? candidate.end : candidate.start;
                advanced = true;
                break;
            }
            if (!advanced)
                return Failure(ProfileError::OpenLoop,
                               "a loop starting at entity " + IdText(loop.entities.front().entityId) +
                                   " does not close");
        }
        loops.push_back(std::move(loop));
    }

    // 6. Every closed curve is a loop on its own. A circle inside a rectangle
    //    is the commonest hole there is, and refusing the combination -- which
    //    is what M4 did -- refused the commonest profile in mechanical CAD.
    for (const SketchEntityId id : closedCurves)
        loops.push_back(ProfileLoop{{OrientedSketchEntityRef{id, false}}});

    if (loops.empty())
        return Failure(ProfileError::NoEntities, "sketch contains no curves to form a profile");

    // 7. Self-intersection, per loop (spec 10): reject rather than guess which
    //    region a crossing outline means.
    //
    //    Scope, stated rather than implied: this tests straight segments
    //    against straight segments. Pairs involving an arc are NOT tested,
    //    because approximating an arc by its chord would report crossings that
    //    do not exist and reject valid profiles -- a false rejection is worse
    //    than the deferred check. Arc-involved self-intersection is a declared
    //    limitation; OCCT still refuses to build a face it cannot make, so the
    //    failure is structured rather than a wrong solid.
    for (const ProfileLoop& loop : loops) {
        std::vector<std::pair<Vec2, Vec2>> segments; // straight members, loop order
        std::vector<std::size_t> segmentPositions;
        for (std::size_t i = 0; i < loop.entities.size(); ++i) {
            const SketchEntity* entity = sketch.findEntity(loop.entities[i].entityId);
            if (entity == nullptr) continue;
            const auto* line = std::get_if<SketchLine>(&entity->geometry);
            if (line == nullptr) continue;
            segments.push_back(loop.entities[i].reversed
                                   ? std::make_pair(line->end, line->start)
                                   : std::make_pair(line->start, line->end));
            segmentPositions.push_back(i);
        }
        const std::size_t loopSize = loop.entities.size();
        for (std::size_t a = 0; a < segments.size(); ++a) {
            for (std::size_t b = a + 1; b < segments.size(); ++b) {
                // Skip neighbours in the loop: they legitimately share an end.
                const std::size_t pa = segmentPositions[a];
                const std::size_t pb = segmentPositions[b];
                const std::size_t gap = pb - pa;
                if (gap == 1 || gap == loopSize - 1) continue;
                if (!SegmentsProperlyIntersect(segments[a].first, segments[a].second,
                                               segments[b].first, segments[b].second))
                    continue;
                return Failure(ProfileError::SelfIntersecting,
                               "entities " + IdText(loop.entities[pa].entityId) + " and " +
                                   IdText(loop.entities[pb].entityId) + " intersect");
            }
        }
    }

    ProfileResult result;
    if (loops.size() == 1) {
        result.profile.outer = std::move(loops.front());
        return result;
    }

    // 8. WHICH LOOP IS THE OUTER ONE -- decided by containment, never by size
    //    or draw order. Exactly one loop must contain all the others.
    //
    //    Loops that CROSS are refused: a boundary half inside another is not a
    //    hole, an island, or anything else this can build, and picking a
    //    reading for the user would be inventing their intent.
    std::vector<std::vector<Vec2>> polygons;
    polygons.reserve(loops.size());
    for (const ProfileLoop& loop : loops) polygons.push_back(LoopPolygon(sketch, loop));
    for (const std::vector<Vec2>& polygon : polygons)
        if (polygon.size() < 3)
            return Failure(ProfileError::NotChainable,
                           "a loop is too small to enclose any area");

    std::size_t outerIndex = loops.size();
    for (std::size_t i = 0; i < loops.size(); ++i) {
        bool containsEveryOther = true;
        for (std::size_t j = 0; j < loops.size() && containsEveryOther; ++j) {
            if (i == j) continue;
            bool crosses = false;
            const bool inside = PolygonInsidePolygon(polygons[j], polygons[i], crosses);
            if (crosses)
                return Failure(ProfileError::SelfIntersecting,
                               "two loops in this sketch cross each other, so neither is a "
                               "boundary of the other");
            if (!inside) containsEveryOther = false;
        }
        if (!containsEveryOther) continue;
        if (outerIndex != loops.size())
            return Failure(ProfileError::NotNested,
                           "more than one loop contains all the others");
        outerIndex = i;
    }
    if (outerIndex == loops.size())
        return Failure(ProfileError::NotNested,
                       "this sketch has " + std::to_string(loops.size()) +
                           " separate loops and none of them contains the rest -- a profile "
                           "needs one outer boundary, with any others inside it as holes");

    // 9. Holes may not contain each other. An island inside a hole is a
    //    different feature (a second solid region), and building it as a plain
    //    hole would quietly lose it.
    for (std::size_t i = 0; i < loops.size(); ++i) {
        if (i == outerIndex) continue;
        for (std::size_t j = 0; j < loops.size(); ++j) {
            if (j == outerIndex || i == j) continue;
            bool crosses = false;
            if (PolygonInsidePolygon(polygons[j], polygons[i], crosses))
                return Failure(ProfileError::NotNested,
                               "one hole contains another; an island inside a hole is not "
                               "something this profile can express");
        }
    }

    result.profile.outer = std::move(loops[outerIndex]);
    for (std::size_t i = 0; i < loops.size(); ++i)
        if (i != outerIndex) result.profile.inners.push_back(std::move(loops[i]));
    return result;
}


bool BuildKernelProfile(const Sketch& sketch, const ValidatedProfile& validated,
                        PlanarProfileDefinition& out) {
    return BuildKernelProfile(sketch, validated, sketch.frame(), out);
}

ProfilePlane PlaneOfSketchFrame(const SketchFrame& frame) noexcept {
    ProfilePlane plane;
    plane.origin = frame.toWorld(Vec2{0.0, 0.0});
    plane.uAxis = frame.uAxis();
    plane.vAxis = frame.vAxis();
    plane.normal = frame.normal();
    return plane;
}

bool BuildKernelProfile(const Sketch& sketch, const ValidatedProfile& validated,
                        const SketchFrame& frame, PlanarProfileDefinition& out) {
    out.plane = PlaneOfSketchFrame(frame);
    out.segments.clear();
    out.innerLoops.clear();

    if (!TranslateChain(sketch, validated.outer, out.segments)) return false;
    for (const ProfileLoop& inner : validated.inners) {
        out.innerLoops.emplace_back();
        if (!TranslateChain(sketch, inner, out.innerLoops.back())) return false;
    }
    return true;
}

PathResult BuildPath(const Sketch& sketch) {
    const auto refuse = [](ProfileError error, std::string message) {
        return PathResult{ValidatedPath{}, error, std::move(message)};
    };

    // 1. The same partition a profile makes. Points and construction geometry
    //    are in the drawing to be measured from, not to be swept along.
    std::vector<Curve> curves;
    std::vector<SketchEntityId> closedCurves;
    for (const SketchEntity& entity : sketch.entities()) {
        if (std::holds_alternative<SketchPoint>(entity.geometry)) continue;
        if (entity.construction) continue;
        if (!IsValidSketchGeometry(entity.geometry))
            return refuse(ProfileError::InvalidGeometry,
                          "entity " + IdText(entity.id) + " has invalid geometry");
        if (!HasEndpoints(entity.geometry)) {
            closedCurves.push_back(entity.id);
            continue;
        }
        curves.push_back(Curve{entity.id, StartPointOf(entity.geometry),
                               EndPointOf(entity.geometry), false});
    }

    // 2. A closed curve IS a path -- a pipe round a ring is an ordinary thing
    //    to want -- but it is a whole one, so nothing may be chained to it.
    if (!closedCurves.empty()) {
        if (closedCurves.size() > 1 || !curves.empty())
            return refuse(ProfileError::NotChainable,
                          "a closed curve is a complete path on its own and cannot be "
                          "chained with anything else");
        ValidatedPath only;
        only.chain.entities.push_back(OrientedSketchEntityRef{closedCurves.front(), false});
        only.closed = true;
        return PathResult{std::move(only), ProfileError::None, {}};
    }
    if (curves.empty())
        return refuse(ProfileError::NoEntities, "sketch contains no curves to form a path");

    std::sort(curves.begin(), curves.end(),
              [](const Curve& a, const Curve& b) { return ToObjectId(a.id) < ToObjectId(b.id); });

    // 3. Duplicates, by the same whole-curve comparison a profile uses: two
    //    entities lying on top of each other make the spine ambiguous.
    for (std::size_t i = 0; i < curves.size(); ++i) {
        for (std::size_t j = i + 1; j < curves.size(); ++j) {
            const SketchEntity* first = sketch.findEntity(curves[i].id);
            const SketchEntity* second = sketch.findEntity(curves[j].id);
            if (first == nullptr || second == nullptr) continue;
            if (!IsSameCurve(first->geometry, second->geometry, kProfileConnectivityToleranceMm))
                continue;
            return refuse(ProfileError::DuplicateEntity,
                          "entities " + IdText(curves[i].id) + " and " + IdText(curves[j].id) +
                              " describe the same curve");
        }
    }

    // 4. Incidence -- and THIS is where a path stops being a profile.
    //
    //    A loop needs every junction to join exactly two ends. A path allows
    //    exactly TWO junctions of degree one: its two ends. Anything else is
    //    still refused, because a branch means there are several spines and
    //    picking one would be a guess about what the user drew.
    std::vector<Junction> junctions;
    for (const Curve& curve : curves) {
        ++junctions[JunctionIndexFor(junctions, curve.start)].incidence;
        ++junctions[JunctionIndexFor(junctions, curve.end)].incidence;
    }
    int ends = 0;
    Vec2 firstEnd{};
    for (const Junction& junction : junctions) {
        if (junction.incidence == 2) continue;
        if (junction.incidence > 2)
            return refuse(ProfileError::Branch,
                          "path branches: an endpoint near (" +
                              std::to_string(junction.position.x) + ", " +
                              std::to_string(junction.position.y) + ") is used by " +
                              std::to_string(junction.incidence) + " curve ends");
        // WHICH END TO START FROM, decided by POSITION rather than by which
        // junction was met first.
        //
        // Met-first depends on the order the curves were sorted in, which is
        // the order of their ids -- so the same drawing produced a spine
        // running one way or the other depending on which line the user
        // happened to draw first. A sweep places its section at the spine's
        // START, so that is not a cosmetic difference: a section that is not
        // centred on the spine builds a different solid.
        //
        // Lexicographic on (u, v) is arbitrary but it is a property of the
        // DRAWING, and that is the whole difference.
        if (ends == 0 || junction.position.x < firstEnd.x ||
            (junction.position.x == firstEnd.x && junction.position.y < firstEnd.y))
            firstEnd = junction.position;
        ++ends;
    }
    if (ends != 0 && ends != 2)
        return refuse(ProfileError::OpenLoop,
                      "a path has two ends or none; this one has " + std::to_string(ends));

    // 5. Walk it. From the lower-id curve at an END for an open path, and from
    //    the lowest id outright for a closed one -- so the answer does not
    //    depend on the order entities were added in.
    ValidatedPath out;
    out.closed = ends == 0;
    Curve* seed = nullptr;
    bool seedReversed = false;
    for (Curve& candidate : curves) {
        if (out.closed) {
            seed = &candidate;
            break;
        }
        if (SamePoint(candidate.start, firstEnd, kProfileConnectivityToleranceMm)) {
            seed = &candidate;
            seedReversed = false;
            break;
        }
        if (SamePoint(candidate.end, firstEnd, kProfileConnectivityToleranceMm)) {
            seed = &candidate;
            seedReversed = true;
            break;
        }
    }
    if (seed == nullptr) return refuse(ProfileError::NotChainable, "path has no starting curve");

    seed->used = true;
    out.chain.entities.push_back(OrientedSketchEntityRef{seed->id, seedReversed});
    Vec2 cursor = seedReversed ? seed->start : seed->end;
    std::size_t used = 1;
    while (used < curves.size()) {
        bool advanced = false;
        for (Curve& candidate : curves) {
            if (candidate.used) continue;
            const bool forward =
                SamePoint(candidate.start, cursor, kProfileConnectivityToleranceMm);
            const bool backward =
                SamePoint(candidate.end, cursor, kProfileConnectivityToleranceMm);
            if (!forward && !backward) continue;
            candidate.used = true;
            ++used;
            out.chain.entities.push_back(OrientedSketchEntityRef{candidate.id, backward});
            cursor = forward ? candidate.end : candidate.start;
            advanced = true;
            break;
        }
        // EVERY CURVE, or none of them. Incidence above proved there are no
        // branches and at most two ends, so a walk that runs out early means
        // the sketch holds a SECOND, separate chain -- and "which of these did
        // you mean" has no defensible default.
        if (!advanced)
            return refuse(ProfileError::Disconnected,
                          "the sketch holds more than one chain; a sweep follows one spine");
    }

    return PathResult{std::move(out), ProfileError::None, {}};
}

std::vector<Vec2> PathPolyline(const Sketch& sketch, const ValidatedPath& path) {
    return LoopPolygonImpl(sketch, path.chain, path.closed);
}

bool BuildKernelPath(const Sketch& sketch, const ValidatedPath& validated,
                     const SketchFrame& frame, PlanarPathDefinition& out) {
    out.plane = PlaneOfSketchFrame(frame);
    out.segments.clear();
    out.closed = validated.closed;
    return TranslateChain(sketch, validated.chain, out.segments);
}

} // namespace paramcad
