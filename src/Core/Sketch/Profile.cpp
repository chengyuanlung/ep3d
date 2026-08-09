#include "Core/Sketch/Profile.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
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

} // namespace

ProfileResult BuildProfile(const Sketch& sketch) {
    // 1. Partition entities. Points are reference geometry: they cannot
    //    contribute an edge, so they are ignored rather than rejected.
    std::vector<Curve> curves;
    std::vector<SketchEntityId> closedCurves; // full circles
    for (const SketchEntity& entity : sketch.entities()) {
        if (std::holds_alternative<SketchPoint>(entity.geometry)) continue;

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
    if (!closedCurves.empty()) {
        if (closedCurves.size() > 1 || !curves.empty())
            return Failure(ProfileError::NotChainable,
                           "closed curve " + IdText(closedCurves.front()) +
                               " cannot be combined with other entities in one loop");
        ProfileResult result;
        result.profile.outer.entities.push_back(
            OrientedSketchEntityRef{closedCurves.front(), false});
        return result;
    }

    if (curves.empty())
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

    // 5. Walk the loop from the lowest id, orienting each curve so the loop
    //    reads start-to-end.
    ProfileLoop loop;
    curves.front().used = true;
    loop.entities.push_back(OrientedSketchEntityRef{curves.front().id, false});
    const Vec2 loopStart = curves.front().start;
    Vec2 cursor = curves.front().end;

    for (std::size_t step = 1; step < curves.size(); ++step) {
        bool advanced = false;
        for (Curve& candidate : curves) {
            if (candidate.used) continue;
            const bool forward =
                SamePoint(candidate.start, cursor, kProfileConnectivityToleranceMm);
            const bool backward =
                SamePoint(candidate.end, cursor, kProfileConnectivityToleranceMm);
            if (!forward && !backward) continue;
            candidate.used = true;
            loop.entities.push_back(OrientedSketchEntityRef{candidate.id, backward});
            cursor = forward ? candidate.end : candidate.start;
            advanced = true;
            break;
        }
        // Incidence already proved every junction has degree two, so failing
        // to advance means the remaining curves form a separate component.
        if (!advanced)
            return Failure(ProfileError::Disconnected,
                           "profile has more than one connected component");
    }

    if (!SamePoint(cursor, loopStart, kProfileConnectivityToleranceMm))
        return Failure(ProfileError::OpenLoop, "profile does not return to its starting point");

    if (std::any_of(curves.begin(), curves.end(), [](const Curve& c) { return !c.used; }))
        return Failure(ProfileError::Disconnected,
                       "profile has more than one connected component");

    // 6. Self-intersection (spec 10): reject rather than guess which region a
    //    crossing outline means.
    //
    //    Scope, stated rather than implied: this tests straight segments
    //    against straight segments. Pairs involving an arc are NOT tested,
    //    because approximating an arc by its chord would report crossings that
    //    do not exist and reject valid profiles -- a false rejection is worse
    //    than the deferred check. Arc-involved self-intersection is a declared
    //    M4 limitation; OCCT still refuses to build a face it cannot make, so
    //    the failure is structured rather than a wrong solid.
    std::vector<std::pair<Vec2, Vec2>> segments; // straight members, loop order
    std::vector<std::size_t> segmentPositions;
    for (std::size_t i = 0; i < loop.entities.size(); ++i) {
        const SketchEntity* entity = sketch.findEntity(loop.entities[i].entityId);
        if (entity == nullptr) continue;
        const auto* line = std::get_if<SketchLine>(&entity->geometry);
        if (line == nullptr) continue;
        segments.push_back(loop.entities[i].reversed ? std::make_pair(line->end, line->start)
                                                     : std::make_pair(line->start, line->end));
        segmentPositions.push_back(i);
    }
    const std::size_t loopSize = loop.entities.size();
    for (std::size_t a = 0; a < segments.size(); ++a) {
        for (std::size_t b = a + 1; b < segments.size(); ++b) {
            // Skip neighbours in the loop: they legitimately share an endpoint.
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

    ProfileResult result;
    result.profile.outer = std::move(loop);
    return result;
}

} // namespace paramcad
