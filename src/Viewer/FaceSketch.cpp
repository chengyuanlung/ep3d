#include "Viewer/FaceSketch.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace paramcad {

namespace {

double Dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 Cross(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

bool Finite(Vec3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// How nearly parallel to world +Z the normal has to be before "v points up"
// stops having an answer. cos(2.5 degrees) or so: well inside the range where
// a user would call the face horizontal, and far outside the range where the
// cross product loses precision.
constexpr double kHorizontalCosine = 0.999;

// How parallel a curve's own plane must be to the sketch's before its
// projection is still a circle rather than an ellipse. cos(~1.4 degrees):
// tight enough that no visibly tilted circle slips through, loose enough to
// absorb the rounding a kernel's axis carries.
constexpr double kCoplanarCosine = 0.9997;

// Two projected points are the same vertex within this distance, in mm. The
// sketch model's own tolerance -- a face's corners are exactly shared between
// its edges, so this only has to absorb arithmetic, and a looser value would
// start merging genuinely distinct vertices on small features.
constexpr double kSameVertexMm = kSketchToleranceMm;

Vec2 ToSketch(const SketchFrame& frame, Vec3 world) {
    // Orthogonal projection onto the plane, expressed in the plane's own axes.
    // The frame's axes are unit and square (SketchFrame::FromBasis guarantees
    // it), so the dot products ARE the coordinates -- there is no inverse to
    // compute, and no second convention that could drift out of step with
    // SketchFrame::toWorld.
    const Vec3 origin = frame.toWorld(Vec2{0.0, 0.0});
    const Vec3 u = frame.uAxis();
    const Vec3 v = frame.vAxis();
    const Vec3 d{world.x - origin.x, world.y - origin.y, world.z - origin.z};
    return Vec2{Dot(d, u), Dot(d, v)};
}

double Separation(Vec2 a, Vec2 b) noexcept {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
}

// Collects vertices without duplicating the ones neighbouring edges share.
class VertexSet {
public:
    void add(Vec2 point) {
        for (Vec2 existing : points_)
            if (Separation(existing, point) <= kSameVertexMm) return;
        points_.push_back(point);
    }
    const std::vector<Vec2>& points() const noexcept { return points_; }

private:
    std::vector<Vec2> points_;
};

} // namespace

ProjectedBoundary ProjectBoundaryOntoSketch(const FaceBoundary& boundary,
                                            const SketchFrame& frame) {
    ProjectedBoundary result;
    const Vec3 n = frame.normal();
    VertexSet vertices;
    // Counted apart because the three have different answers for the user: a
    // spline edge is waiting on a milestone, a tilted circle usually means the
    // sketch is on a plane the hole is not parallel to, and an edge-on line
    // means the face is being viewed from its own side.
    int unsupportedKind = 0;
    int tilted = 0;
    int degenerate = 0;

    for (const FaceCurve& curve : boundary) {
        switch (curve.kind) {
            case FaceCurve::Kind::Line: {
                const Vec2 a = ToSketch(frame, curve.start);
                const Vec2 b = ToSketch(frame, curve.end);
                if (Separation(a, b) <= kSketchToleranceMm) {
                    // Perpendicular to the plane: the whole edge lands on one
                    // point. The point is still worth keeping as a snap target,
                    // but the edge is gone and is counted as gone.
                    ++degenerate;
                    vertices.add(a);
                    break;
                }
                result.geometry.push_back(SketchLine{a, b});
                vertices.add(a);
                vertices.add(b);
                break;
            }
            case FaceCurve::Kind::Circle:
            case FaceCurve::Kind::Arc: {
                const double alignment = Dot(curve.axis, n);
                if (std::fabs(alignment) < kCoplanarCosine) {
                    ++tilted; // an ellipse, and EP3D has no ellipse entity
                    break;
                }
                if (curve.radiusMm <= kSketchToleranceMm) {
                    ++degenerate;
                    break;
                }
                const Vec2 centre = ToSketch(frame, curve.center);
                vertices.add(centre);
                if (curve.kind == FaceCurve::Kind::Circle) {
                    result.geometry.push_back(SketchCircle{centre, curve.radiusMm});
                    break;
                }
                const Vec2 start = ToSketch(frame, curve.start);
                const Vec2 end = ToSketch(frame, curve.end);
                double startAngle = std::atan2(start.y - centre.y, start.x - centre.x);
                double endAngle = std::atan2(end.y - centre.y, end.x - centre.x);
                // Stored counter-clockwise ALWAYS, by swapping the tips when
                // the curve's own axis opposes the sketch normal. An arc from A
                // to B clockwise is the same curve as B to A counter-clockwise,
                // so nothing is lost -- and every arc in the underlay is then
                // stored the one way the rest of the sketch model expects,
                // rather than carrying a direction every consumer must honour.
                if (alignment < 0.0) std::swap(startAngle, endAngle);
                result.geometry.push_back(
                    SketchArc{centre, curve.radiusMm, startAngle, endAngle, true});
                vertices.add(start);
                vertices.add(end);
                break;
            }
            case FaceCurve::Kind::Unsupported:
                ++unsupportedKind;
                break;
        }
    }

    for (Vec2 point : vertices.points()) result.geometry.push_back(SketchPoint{point});

    result.skipped = unsupportedKind + tilted + degenerate;
    if (result.skipped > 0) {
        std::string parts;
        const auto add = [&parts](int count, const char* what) {
            if (count <= 0) return;
            if (!parts.empty()) parts += ", ";
            parts += std::to_string(count) + " " + what;
        };
        add(unsupportedKind, "spline or ellipse");
        add(tilted, "circle seen at an angle");
        add(degenerate, "edge-on");
        result.skippedReason =
            std::to_string(result.skipped) + " edge(s) not projected (" + parts + ")";
    }
    return result;
}

FaceSketchPlan PlanSketchOnFace(const PickedFace& face) {
    FaceSketchPlan plan;

    if (!face.isFace) {
        plan.message = "Click a flat face of a solid first";
        return plan;
    }
    // Named for what it is. "Invalid selection" would leave a user clicking the
    // same rounded face again, wondering what they did wrong.
    if (!face.planar) {
        plan.message = "That face is curved. A sketch needs a flat face";
        return plan;
    }
    if (!Finite(face.point) || !Finite(face.normal)) {
        plan.message = "That face's geometry could not be read";
        return plan;
    }

    const double normalLength = std::sqrt(Dot(face.normal, face.normal));
    if (!std::isfinite(normalLength) || normalLength < 1e-12) {
        plan.message = "That face has no usable direction";
        return plan;
    }
    const Vec3 n{face.normal.x / normalLength, face.normal.y / normalLength,
                 face.normal.z / normalLength};

    // THE conversion site, in Core (M17.14). It used to be spelled out here,
    // and then recompute needed the same convention in order to re-resolve a
    // tracked face -- two copies of "which way is up on this face" would let a
    // sketch jump the first time it was rebuilt.
    const std::optional<SketchFrame> frame = SketchFrame::OnFace(face.point, n);
    if (!frame) {
        // Reachable only if the basis degenerates after all the checks above,
        // which would mean the kernel handed back something inconsistent. Said
        // out loud rather than papered over with world XY.
        plan.message = "That face's plane could not be turned into a sketch frame";
        return plan;
    }

    plan.ok = true;
    plan.frame = *frame;
    plan.reference = ProjectBoundaryOntoSketch(face.boundary, *frame);
    plan.message = "Sketch placed on the picked face's plane. It stays on this plane; "
                   "it does not follow the face";
    // The underlay is part of what the command did, so it is part of what the
    // command says. A count the user can compare against the face they clicked
    // is the only way to tell a complete underlay from a partial one.
    if (!plan.reference.geometry.empty())
        plan.message += ". " + std::to_string(plan.reference.geometry.size()) +
                        " reference item(s) projected";
    if (plan.reference.skipped > 0) plan.message += "; " + plan.reference.skippedReason;
    return plan;
}

} // namespace paramcad
