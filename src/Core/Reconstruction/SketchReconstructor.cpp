#include "Core/Reconstruction/SketchReconstructor.h"

#include "Core/Document/PartDocument.h"
#include "Core/Parameter/ParameterManager.h"
#include "Core/Sketch/Sketch.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>

namespace paramcad {
namespace {

// One line of the sketch, with its identity. Collected once so nothing below
// walks `entities()` repeatedly, and -- more importantly -- so nothing below is
// tempted to refer to a line by its position in that vector (spec 7).
struct LineRef {
    SketchEntityId id{kInvalidSketchEntityId};
    Vec2 start{};
    Vec2 end{};

    Vec2 point(SketchSubElement which) const noexcept {
        return which == SketchSubElement::StartPoint ? start : end;
    }
};

std::vector<LineRef> CollectLines(const Sketch& sketch) {
    std::vector<LineRef> lines;
    for (const SketchEntity& entity : sketch.entities()) {
        if (const auto* line = std::get_if<SketchLine>(&entity.geometry))
            lines.push_back(LineRef{entity.id, line->start, line->end});
    }
    return lines;
}

bool Near(Vec2 a, Vec2 b, double toleranceMm) noexcept {
    return SamePoint(a, b, toleranceMm);
}

// A circle or arc, with its identity. Radius and Diameter dimensions target
// these, and both kinds drive the SAME underlying radius (spec 12): the model
// has one geometric radius state and Diameter is a second view of it, not a
// second quantity.
struct CurveRef {
    SketchEntityId id{kInvalidSketchEntityId};
    Vec2 center{};
    double radiusMm{0.0};
};

std::vector<CurveRef> CollectCurves(const Sketch& sketch) {
    std::vector<CurveRef> curves;
    for (const SketchEntity& entity : sketch.entities()) {
        if (const auto* circle = std::get_if<SketchCircle>(&entity.geometry))
            curves.push_back(CurveRef{entity.id, circle->center, circle->radiusMm});
        // Arcs carry a radius too, and a radial dimension on a fillet is
        // ordinary draughting. Leaving them out would reconstruct a drawing's
        // holes and silently ignore its rounds.
        else if (const auto* arc = std::get_if<SketchArc>(&entity.geometry))
            curves.push_back(CurveRef{entity.id, arc->center, arc->radiusMm});
    }
    return curves;
}

double LengthOf(const LineRef& line) noexcept {
    const double du = line.end.x - line.start.x;
    const double dv = line.end.y - line.start.y;
    return std::sqrt(du * du + dv * dv);
}

// Whether a line lies within `toleranceRad` of an axis.
//
// Measured as the perpendicular offset over the length -- the sine of the
// angle -- rather than by calling atan2 and comparing angles. atan2 needs the
// direction normalised into a half-turn first (a line from right to left is
// just as horizontal as one from left to right), and getting that wrong is
// silent: it recognises half the lines in any drawing and no test that draws
// its rectangle one way round can see it.
bool IsHorizontal(const LineRef& line, double toleranceRad) noexcept {
    const double length = LengthOf(line);
    if (length <= 0.0) return false;
    return std::abs(line.end.y - line.start.y) / length <= std::sin(toleranceRad);
}

bool IsVertical(const LineRef& line, double toleranceRad) noexcept {
    const double length = LengthOf(line);
    if (length <= 0.0) return false;
    return std::abs(line.end.x - line.start.x) / length <= std::sin(toleranceRad);
}

// Lexicographic order on a point, used wherever a deterministic CHOICE has to
// be made among candidates -- which corner to Fix, which dimension is Width.
//
// Geometric, so it does not depend on the order entities arrived in. That is
// the property the shuffled-order test checks, and an ordering by entity id or
// vector position would fail it while looking perfectly deterministic.
bool Before(Vec2 a, Vec2 b) noexcept {
    if (a.x != b.x) return a.x < b.x;
    return a.y < b.y;
}

// --- General endpoint clustering (M7.2, ADR-M7-011) ------------------------
//
// Every endpoint in the sketch, grouped by location. This replaces M7.1's
// named-rectangle rule: the rectangle now falls OUT of the general rules
// rather than being recognised as a shape, which is what spec 38 asks M7.2 for.
//
// Clustering rather than pairwise matching, because clustering is what makes
// junctions describable at all. Three lines meeting at a point is a cluster of
// three; pairwise matching sees three separate "matches" and cannot tell that
// from a triangle whose corners happen to be near each other.
struct Endpoint {
    SketchElementRef ref{};
    Vec2 at{};
    // The owning entity's midpoint, used only for deterministic ORDER. It is a
    // geometric property, so the order does not change when the file lists the
    // same entities differently -- which an ordering by entity id would, since
    // ids are handed out in file order.
    Vec2 owner{};
};

struct EndpointCluster {
    Vec2 at{};
    std::vector<Endpoint> members;
};

std::vector<Endpoint> CollectEndpoints(const Sketch& sketch) {
    std::vector<Endpoint> endpoints;
    for (const SketchEntity& entity : sketch.entities()) {
        // Points and circles have no endpoints a coincidence could join
        // (HasEndpoints is the model's own answer, so this cannot drift from
        // it). Arcs DO, and their endpoints are as joinable as a line's --
        // leaving them out would silently refuse to close any profile with a
        // rounded corner in it.
        if (!HasEndpoints(entity.geometry)) continue;
        const Vec2 mid = MidPointOf(entity.geometry);
        endpoints.push_back(Endpoint{SketchElementRef{entity.id, SketchSubElement::StartPoint},
                                     StartPointOf(entity.geometry), mid});
        endpoints.push_back(Endpoint{SketchElementRef{entity.id, SketchSubElement::EndPoint},
                                     EndPointOf(entity.geometry), mid});
    }
    return endpoints;
}

std::vector<EndpointCluster> ClusterEndpoints(const std::vector<Endpoint>& endpoints,
                                              double toleranceMm) {
    std::vector<EndpointCluster> clusters;
    for (const Endpoint& endpoint : endpoints) {
        EndpointCluster* found = nullptr;
        for (EndpointCluster& cluster : clusters)
            if (Near(cluster.at, endpoint.at, toleranceMm)) found = &cluster;
        if (found == nullptr) {
            clusters.push_back(EndpointCluster{endpoint.at, {}});
            found = &clusters.back();
        }
        found->members.push_back(endpoint);
    }

    // Deterministic throughout: clusters by location, members by their owning
    // entity's midpoint and then by which end. Nothing downstream may depend on
    // the order entities were stored in (ADR-M6-004 is the same rule).
    for (EndpointCluster& cluster : clusters)
        std::sort(cluster.members.begin(), cluster.members.end(),
                  [](const Endpoint& a, const Endpoint& b) {
                      if (a.owner.x != b.owner.x || a.owner.y != b.owner.y)
                          return Before(a.owner, b.owner);
                      return a.ref.subElement < b.ref.subElement;
                  });
    std::sort(clusters.begin(), clusters.end(),
              [](const EndpointCluster& a, const EndpointCluster& b) {
                  return Before(a.at, b.at);
              });
    return clusters;
}

// --- Naming (spec 9) --------------------------------------------------------

bool NameIsTaken(const PartDocument& document, const ReconstructionPlan& plan,
                 const std::string& name) {
    if (document.parameters().findByName(name) != nullptr) return true;
    for (const PlannedParameter& planned : plan.parameters)
        if (planned.name == name) return true;
    return false;
}

// `preferred`, or `preferred_2`, `preferred_3`, ... -- the first that is free.
//
// Deterministic and documented (spec 9), and it looks at BOTH the document and
// the plan, because two names invented in one analysis collide with each other
// long before either reaches the document.
std::string FreeName(const PartDocument& document, const ReconstructionPlan& plan,
                     const std::string& preferred) {
    if (!NameIsTaken(document, plan, preferred)) return preferred;
    // Bounded: a document with 999 Parameters called Width has a problem this
    // loop cannot fix, and spinning forever is not a better answer.
    for (int suffix = 2; suffix < 1000; ++suffix) {
        std::string candidate = preferred + "_" + std::to_string(suffix);
        if (!NameIsTaken(document, plan, candidate)) return candidate;
    }
    return {};
}

// --- Dimension association (spec 8) -----------------------------------------

struct Candidate {
    const ImportedDimension2D* dimension{nullptr};
    double valueMm{0.0};
    SketchEntityId target{kInvalidSketchEntityId};
    bool targetIsHorizontal{false};
    Vec2 sortKey{};
};

// A resolved Radius/Diameter dimension, held until naming. Parallel to
// `Candidate` above, and deliberately so: the two kinds are named by different
// rules but must both be ordered geometrically before naming.
struct CurveCandidate {
    const ImportedDimension2D* dimension{nullptr};
    double valueMm{0.0};
    SketchEntityId target{kInvalidSketchEntityId};
    bool diameter{false};
    Vec2 center{};
    double radiusMm{0.0};
};

std::string Describe(const ImportedDimension2D& dimension) {
    std::string text = std::string(ImportedDimensionKindName(dimension.kind)) + " dimension";
    if (!dimension.sourceHandle.empty()) text += " (handle " + dimension.sourceHandle + ")";
    return text;
}

} // namespace

ReconstructionPlan AnalyzeForReconstruction(const PartDocument& document, ObjectId sketchId,
                                            const std::vector<ImportedDimension2D>& dimensions,
                                            const ReconstructionOptions& options) {
    ReconstructionPlan plan;
    const Sketch* sketch = document.findSketch(sketchId);
    if (sketch == nullptr) return plan;
    plan.documentId = document.id();
    plan.sketchId = sketchId;
    // Content identity, stamped at analysis time (round 2's C1): the ids above
    // are a process-local counter and cannot separate two documents.
    plan.fingerprint = FingerprintSketch(*sketch);

    const std::vector<LineRef> lines = CollectLines(*sketch);
    const std::vector<CurveRef> curves = CollectCurves(*sketch);
    const std::vector<EndpointCluster> clusters =
        ClusterEndpoints(CollectEndpoints(*sketch), options.coincidenceToleranceMm);

    // --- Geometric relations (spec 8 Class C, spec 11) ----------------------
    //
    // General rules over whatever geometry is present, not a named shape. The
    // rectangle now falls out of these rather than being special-cased: four
    // clusters of two give four Coincidents, two sides each land on an axis,
    // and one Fix pins the whole thing.
    if (options.recognizeCoincident) {
        for (const EndpointCluster& cluster : clusters) {
            // A SPANNING set, not every pair. n endpoints at one location need
            // n-1 constraints to be tied together; all n(n-1)/2 pairs say the
            // same thing with (n-1)(n-2)/2 redundant residuals.
            //
            // WHEN THAT ACTUALLY BITES, measured rather than assumed. An
            // earlier version of this comment claimed all-pairs turns a
            // three-line junction into an OverConstrained sketch. It does not,
            // and the integration test proved it: M5 gives DOF priority over
            // redundancy (GaussNewtonSketchSolver, "DOF OUTRANKS REDUNDANCY"),
            // so while any freedom remains the sketch reports UnderConstrained
            // and the redundant rows are invisible.
            //
            // The masking ends exactly when dimensions bring the sketch to
            // DOF 0 -- then `redundant` becomes the headline and a fully
            // dimensioned part containing one T-junction reports
            // OverConstrained instead of Solved. So the harm is real but
            // deferred, which is the worst shape for a defect: it appears when
            // the user finishes constraining, not when the junction is made.
            //
            // For the two-member clusters of a rectangle the two constructions
            // are identical, which is why this difference has to be tested on a
            // junction and cannot be seen on the release fixture at all.
            for (std::size_t i = 1; i < cluster.members.size(); ++i) {
                if (cluster.members[i].ref.entityId == cluster.members[0].ref.entityId)
                    // Both ends of ONE entity in the same cluster: a closed or
                    // degenerate curve. Constraining it to itself says nothing
                    // and the solver would carry a residual that is always
                    // zero.
                    continue;
                plan.constraints.push_back(PlannedConstraint{
                    CoincidentConstraint{cluster.members[0].ref, cluster.members[i].ref},
                    kInvalidPlanParameterSlot, ReconstructionOrigin::InferredGeometric,
                    "endpoints within the coincidence tolerance"});
            }
        }
    }

    for (const LineRef& line : lines) {
        const bool horizontal = IsHorizontal(line, options.axisAngleToleranceRad);
        const bool vertical = IsVertical(line, options.axisAngleToleranceRad);
        // A line inside BOTH tolerances is shorter than the tolerance can
        // resolve -- its direction is noise. Asserting either axis about it
        // would be a coin toss, and asserting both would be a contradiction the
        // solver reports as Conflicting on geometry that is merely tiny.
        if (horizontal && vertical) continue;
        if (horizontal && options.recognizeHorizontal)
            plan.constraints.push_back(PlannedConstraint{
                HorizontalConstraint{line.id}, kInvalidPlanParameterSlot,
                ReconstructionOrigin::InferredGeometric, "within the axis tolerance of horizontal"});
        if (vertical && options.recognizeVertical)
            plan.constraints.push_back(PlannedConstraint{
                VerticalConstraint{line.id}, kInvalidPlanParameterSlot,
                ReconstructionOrigin::InferredGeometric, "within the axis tolerance of vertical"});
    }

    if (options.placeFix) {
        // The lexicographically smallest endpoint, so the choice is a fact
        // about the geometry and not about the order it was read in
        // (ADR-M7-008). Without a Fix the sketch keeps its two global
        // translation freedoms and can never reach DOF 0, however many
        // dimensions are reconstructed.
        if (!clusters.empty()) {
            plan.constraints.push_back(PlannedConstraint{
                FixConstraint{clusters.front().members.front().ref}, kInvalidPlanParameterSlot,
                ReconstructionOrigin::InferredPlacement, "deterministic placement"});
        } else if (!curves.empty()) {
            // A sketch of nothing but circles has no endpoints at all, and
            // would otherwise never be placeable however many radii were
            // reconstructed -- a hole pattern is exactly that drawing. Its
            // centre is the only anchor a circle has.
            //
            // Endpoints are preferred whenever there are any, so adding this
            // cannot move the Fix on any sketch that already had one.
            const CurveRef* anchor = &curves.front();
            for (const CurveRef& curve : curves)
                if (Before(curve.center, anchor->center)) anchor = &curve;
            plan.constraints.push_back(PlannedConstraint{
                FixConstraint{SketchElementRef{anchor->id, SketchSubElement::CenterPoint}},
                kInvalidPlanParameterSlot, ReconstructionOrigin::InferredPlacement,
                "deterministic placement"});
        }
    }

    if (!options.reconstructExplicitDimensions) return plan;

    // --- Explicit source dimensions (spec 8, Class A / Class B) -------------
    std::vector<Candidate> resolved;
    std::vector<CurveCandidate> resolvedCurves;
    for (const ImportedDimension2D& dimension : dimensions) {
        const std::string what = Describe(dimension);

        const bool linearKind = dimension.kind == ImportedDimensionKind::Linear ||
                                dimension.kind == ImportedDimensionKind::Aligned;
        const bool curveKind = dimension.kind == ImportedDimensionKind::Radius ||
                               dimension.kind == ImportedDimensionKind::Diameter;
        if (!linearKind && !curveKind) {
            plan.skipped.push_back(ReconstructionSkip{
                ReconstructionSkipReason::UnsupportedKind,
                what + " is not reconstructed; M7 supports linear, aligned, radius and "
                       "diameter dimensions",
                dimension.sourceHandle});
            continue;
        }

        // A text override means the drawing SHOWS a number the geometry does
        // not encode. "<>" is the format's way of saying "show the measurement"
        // and is therefore not an override at all.
        if (!dimension.textOverride.empty() && dimension.textOverride != "<>") {
            plan.skipped.push_back(ReconstructionSkip{
                ReconstructionSkipReason::TextOverride,
                what + " displays the text '" + dimension.textOverride +
                    "', which may not be what its geometry measures; M7 does not guess "
                    "which the drawing meant",
                dimension.sourceHandle});
            continue;
        }

        // The value: the definition points are the authority, and a stated
        // code-42 measurement is believed over them when present, because that
        // is the value the drawing was dimensioned TO (ADR-M7-009).
        double value = MeasuredValueMm(dimension);
        if (dimension.statedValueMm.has_value() && *dimension.statedValueMm > 0.0) {
            const double stated = *dimension.statedValueMm;
            const double reference = std::max(std::abs(stated), std::abs(value));
            if (reference > 0.0 &&
                std::abs(stated - value) / reference > options.valueAgreementFraction) {
                plan.skipped.push_back(ReconstructionSkip{
                    ReconstructionSkipReason::ValueDisagreesWithGeometry,
                    what + " states " + std::to_string(stated) + " mm but its definition points "
                           "are " + std::to_string(value) +
                        " mm apart; the drawing disagrees with itself and M7 will not choose "
                        "a side",
                    dimension.sourceHandle});
                continue;
            }
            value = stated;
        }

        if (!std::isfinite(value) || value < kMinReconstructedDimensionMm) {
            plan.skipped.push_back(ReconstructionSkip{
                ReconstructionSkipReason::InvalidValue,
                what + " measures " + std::to_string(value) +
                    " mm, which is not a usable length",
                dimension.sourceHandle});
            continue;
        }

        // --- Curve dimensions: which circle or arc is this about? -----------
        //
        // Matched on the CENTRE and then cross-checked on the radius. Centre
        // alone is not enough: concentric circles are ordinary (a counterbored
        // hole is two), and every one of them would match. Radius alone is not
        // enough either, since a drawing full of identical holes has many.
        if (curveKind) {
            const Vec2 center = DimensionCenter(dimension);
            // The radius the dimension asserts. A Diameter states twice it --
            // the model has ONE radius state and Diameter is a second view of
            // it, never separate geometry (spec 12).
            const double radius =
                dimension.kind == ImportedDimensionKind::Diameter ? value * 0.5 : value;

            std::vector<const CurveRef*> curveMatches;
            for (const CurveRef& curve : curves) {
                if (!Near(curve.center, center, options.coincidenceToleranceMm)) continue;
                const double reference = std::max(radius, curve.radiusMm);
                if (reference > 0.0 &&
                    std::abs(radius - curve.radiusMm) / reference > options.valueAgreementFraction)
                    continue;
                curveMatches.push_back(&curve);
            }

            if (curveMatches.empty()) {
                plan.skipped.push_back(ReconstructionSkip{
                    ReconstructionSkipReason::NoTargetGeometry,
                    what + " is centred where no imported circle or arc of that size is",
                    dimension.sourceHandle});
                continue;
            }
            if (curveMatches.size() > 1) {
                plan.skipped.push_back(ReconstructionSkip{
                    ReconstructionSkipReason::AmbiguousTarget,
                    what + " could refer to " + std::to_string(curveMatches.size()) +
                        " concentric curves of the same size, and nothing in the source "
                        "chooses between them",
                    dimension.sourceHandle});
                continue;
            }

            const CurveRef& target = *curveMatches.front();
            // COLLECTED, not named here. Naming happens after the loop against a
            // geometric sort, exactly as the linear path does -- naming inline
            // made `Radius` and `Radius_2` swap places when the same drawing was
            // exported with its dimensions listed the other way round, which
            // ADR-M7-002 forbids in as many words. The two paths are now
            // structurally parallel; that they were not is why the linear leg
            // had a test and this one did not.
            resolvedCurves.push_back(CurveCandidate{
                &dimension, value, target.id,
                dimension.kind == ImportedDimensionKind::Diameter, target.center,
                target.radiusMm});
            continue;
        }

        // Association: which native line has these two definition points as its
        // endpoints? Semantic, by geometry, never by array position -- the
        // dimension does not name an entity and DXF gives it no way to.
        std::vector<const LineRef*> matches;
        for (const LineRef& line : lines) {
            const bool forward = Near(line.start, dimension.measureFrom, options.coincidenceToleranceMm) &&
                                 Near(line.end, dimension.measureTo, options.coincidenceToleranceMm);
            const bool reversed = Near(line.start, dimension.measureTo, options.coincidenceToleranceMm) &&
                                  Near(line.end, dimension.measureFrom, options.coincidenceToleranceMm);
            if (forward || reversed) matches.push_back(&line);
        }

        if (matches.empty()) {
            plan.skipped.push_back(ReconstructionSkip{
                ReconstructionSkipReason::NoTargetGeometry,
                what + " measures between two points that are not the ends of any imported line",
                dimension.sourceHandle});
            continue;
        }
        if (matches.size() > 1) {
            // Spec 18's leading example. Two identical overlapping lines, or a
            // duplicate export, and nothing in the source says which was meant.
            plan.skipped.push_back(ReconstructionSkip{
                ReconstructionSkipReason::AmbiguousTarget,
                what + " could refer to " + std::to_string(matches.size()) +
                    " different imported lines, and nothing in the source chooses between them",
                dimension.sourceHandle});
            continue;
        }

        const LineRef& target = *matches.front();

        // A Length constraint controls the line's LENGTH. A Linear dimension
        // states the PROJECTION of its two points onto `directionRad`, and those
        // are the same number only when the dimension is measured along the
        // line. For any other direction the value is a different quantity -- a
        // horizontal run, a vertical rise, an offset -- and this model has no
        // constraint that means it.
        //
        // Without this check a perfectly ordinary drawing is silently ruined: a
        // horizontal DIMLINEAR across the ends of a 13 mm sloped line states 5,
        // reconstruction reports success with no diagnostic, and the solver then
        // shortens the line to 5. That is spec 34's first Critical, and
        // MeasuredValueMm's own comment predicted it -- the two kinds are kept
        // apart at the value layer and were being collapsed here, one level up.
        //
        // Aligned is exempt by construction: its direction IS its definition
        // points, so the projection is the separation.
        if (dimension.kind == ImportedDimensionKind::Linear) {
            const double du = target.end.x - target.start.x;
            const double dv = target.end.y - target.start.y;
            const double length = std::sqrt(du * du + dv * dv);
            const double along =
                std::abs(du * std::cos(dimension.directionRad) + dv * std::sin(dimension.directionRad));
            // Compared against the axis tolerance expressed as a shortening
            // ratio, so the same angular allowance that lets a nearly-horizontal
            // side be called Horizontal also lets its dimension be believed.
            if (length <= 0.0 ||
                (length - along) / length > 1.0 - std::cos(options.axisAngleToleranceRad)) {
                plan.skipped.push_back(ReconstructionSkip{
                    ReconstructionSkipReason::ValueDisagreesWithGeometry,
                    what + " measures along a direction the line it names does not follow, so its "
                           "value is a projection rather than that line's length",
                    dimension.sourceHandle});
                continue;
            }
        }

        resolved.push_back(Candidate{&dimension, value, target.id,
                                     IsHorizontal(target, options.axisAngleToleranceRad),
                                     Before(target.start, target.end) ? target.start : target.end});
    }

    // Deterministic naming order, by geometry: horizontal dimensions first,
    // then by location. Naming must not depend on the order dimensions appeared
    // in the file, or the same drawing exported twice would produce Width and
    // Height the other way round.
    // TOTAL, not merely deterministic (round 2's M2). Both keys above describe
    // the TARGET, so two dimensions resolving to one line tie -- and std::sort
    // is unstable, so the file order leaked straight through into which value
    // got called Width. Two callouts on one edge is ordinary draughting, and
    // the suite already treats it as such.
    //
    // The tiebreaks are properties of the DIMENSION, ordered so correctness
    // never rests on the last one: value, then kind, then the source handle.
    // The handle is a last resort only -- if two dimensions agree on target,
    // value AND kind they are interchangeable, and a stable order among
    // interchangeable things is all that is left to want (spec 7).
    std::sort(resolved.begin(), resolved.end(), [](const Candidate& a, const Candidate& b) {
        if (a.targetIsHorizontal != b.targetIsHorizontal) return a.targetIsHorizontal;
        if (a.sortKey.x != b.sortKey.x || a.sortKey.y != b.sortKey.y)
            return Before(a.sortKey, b.sortKey);
        if (a.valueMm != b.valueMm) return a.valueMm < b.valueMm;
        if (a.dimension->kind != b.dimension->kind) return a.dimension->kind < b.dimension->kind;
        return a.dimension->sourceHandle < b.dimension->sourceHandle;
    });

    bool usedWidth = false;
    bool usedHeight = false;
    int genericCount = 0;
    for (const Candidate& candidate : resolved) {
        std::string preferred;
        if (candidate.targetIsHorizontal && !usedWidth) {
            preferred = "Width";
            usedWidth = true;
        } else if (!candidate.targetIsHorizontal && !usedHeight) {
            preferred = "Height";
            usedHeight = true;
        } else {
            preferred = "Length" + std::to_string(++genericCount);
        }

        const std::string name = FreeName(document, plan, preferred);
        if (name.empty()) {
            plan.skipped.push_back(ReconstructionSkip{
                ReconstructionSkipReason::NameCollision,
                "no free Parameter name could be formed from '" + preferred + "'",
                candidate.dimension->sourceHandle});
            continue;
        }

        const auto slot = static_cast<PlanParameterSlot>(plan.parameters.size());
        plan.parameters.push_back(PlannedParameter{name, candidate.valueMm, UnitType::Millimeter,
                                                   ReconstructionOrigin::ExplicitSource,
                                                   candidate.dimension->sourceHandle});
        plan.constraints.push_back(
            PlannedConstraint{LengthConstraint{candidate.target, kInvalidObjectId}, slot,
                              ReconstructionOrigin::ExplicitSource,
                              Describe(*candidate.dimension)});
    }

    // Curve dimensions, ordered by the geometry they name -- centre first, then
    // radius to separate concentric curves. Never by the order they appeared in
    // the file (ADR-M7-002).
    // Total for the same reason as the linear sort above: centre and radius
    // describe the TARGET, so two callouts on one circle tie and the file
    // order decided which one became Radius.
    std::sort(resolvedCurves.begin(), resolvedCurves.end(),
              [](const CurveCandidate& a, const CurveCandidate& b) {
                  if (a.center.x != b.center.x || a.center.y != b.center.y)
                      return Before(a.center, b.center);
                  if (a.radiusMm != b.radiusMm) return a.radiusMm < b.radiusMm;
                  if (a.valueMm != b.valueMm) return a.valueMm < b.valueMm;
                  if (a.diameter != b.diameter) return !a.diameter; // Radius before Diameter
                  return a.dimension->sourceHandle < b.dimension->sourceHandle;
              });

    for (const CurveCandidate& candidate : resolvedCurves) {
        const std::string preferred = candidate.diameter ? "Diameter" : "Radius";
        const std::string name = FreeName(document, plan, preferred);
        if (name.empty()) {
            plan.skipped.push_back(ReconstructionSkip{
                ReconstructionSkipReason::NameCollision,
                "no free Parameter name could be formed from '" + preferred + "'",
                candidate.dimension->sourceHandle});
            continue;
        }

        const auto slot = static_cast<PlanParameterSlot>(plan.parameters.size());
        // The Parameter carries what the DIMENSION says, not the radius: a
        // Diameter parameter reading 20 for a radius-10 hole is what the drawing
        // shows and what the user will edit. The constraint kind is what tells
        // the solver to halve it (ADR-M7-015).
        plan.parameters.push_back(PlannedParameter{name, candidate.valueMm, UnitType::Millimeter,
                                                   ReconstructionOrigin::ExplicitSource,
                                                   candidate.dimension->sourceHandle});
        SketchConstraintData data =
            candidate.diameter
                ? SketchConstraintData{DiameterConstraint{candidate.target, kInvalidObjectId}}
                : SketchConstraintData{RadiusConstraint{candidate.target, kInvalidObjectId}};
        plan.constraints.push_back(PlannedConstraint{std::move(data), slot,
                                                    ReconstructionOrigin::ExplicitSource,
                                                    Describe(*candidate.dimension)});
    }

    return plan;
}

std::uint64_t FingerprintSketch(const Sketch& sketch) {
    // FNV-1a over every entity id and every coordinate. Not cryptographic and
    // does not need to be: it separates documents, and an adversary who can
    // hand-craft a colliding sketch can equally well hand-craft the plan.
    // Doubles are hashed by their exact bit pattern, so "the same drawing"
    // means bit-identical geometry -- which is what a plan built moments ago
    // from that geometry should see.
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) {
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= (value >> (byte * 8)) & 0xFFULL;
            hash *= 1099511628211ULL;
        }
    };
    const auto mixDouble = [&mix](double value) {
        std::uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value), "double is not 64 bits");
        std::memcpy(&bits, &value, sizeof(bits));
        mix(bits);
    };
    for (const SketchEntity& entity : sketch.entities()) {
        mix(static_cast<std::uint64_t>(ToObjectId(entity.id)));
        if (const auto* line = std::get_if<SketchLine>(&entity.geometry)) {
            mix(1);
            mixDouble(line->start.x);
            mixDouble(line->start.y);
            mixDouble(line->end.x);
            mixDouble(line->end.y);
        } else if (const auto* circle = std::get_if<SketchCircle>(&entity.geometry)) {
            mix(2);
            mixDouble(circle->center.x);
            mixDouble(circle->center.y);
            mixDouble(circle->radiusMm);
        } else if (const auto* arc = std::get_if<SketchArc>(&entity.geometry)) {
            mix(3);
            mixDouble(arc->center.x);
            mixDouble(arc->center.y);
            mixDouble(arc->radiusMm);
            mixDouble(arc->startAngleRad);
            mixDouble(arc->endAngleRad);
        } else if (const auto* point = std::get_if<SketchPoint>(&entity.geometry)) {
            mix(4);
            mixDouble(point->position.x);
            mixDouble(point->position.y);
        }
    }
    return hash;
}

PlanValidation ValidatePlanAgainstDocument(const PartDocument& document,
                                           const ReconstructionPlan& plan,
                                           const ReconstructionOptions& options) {
    const Sketch* sketch = document.findSketch(plan.sketchId);
    if (sketch == nullptr)
        return {false, "reconstruction plan names a sketch this document does not have"};

    // IDENTITY first, and by content (round 2's C1). Two parts saved in two
    // sessions share documentId 1 and sketchId 2 by construction, so the ids
    // cannot separate them; the fingerprint can. A plan built before an edit
    // also fails here, which is correct: it no longer describes this sketch.
    if (plan.fingerprint != 0 && plan.fingerprint != FingerprintSketch(*sketch))
        return {false, "this plan was built for different geometry than the sketch now holds; "
                       "re-analyze before applying"};

    // Every planned Parameter NAME must still be free. Checked at plan-build
    // time too, but a name taken in between produced two Parameters called
    // Width driving different geometry -- "every UI that lists them by name
    // would show two identical rows" is this file's own words (round 2's M6).
    for (const PlannedParameter& parameter : plan.parameters)
        if (document.parameters().findByName(parameter.name) != nullptr)
            return {false, "a Parameter named '" + parameter.name +
                               "' already exists in this document; the plan cannot create a "
                               "second one"};

    for (const PlannedConstraint& planned : plan.constraints) {
        for (SketchEntityId target : ReferencedEntities(planned.data)) {
            if (sketch->findEntity(target) == nullptr)
                return {false, std::string("the plan names a ") +
                                   ConstraintKindName(planned.data) +
                                   " on geometry this sketch does not have"};
        }

        // INFERRED constraints are re-asserted against current geometry with
        // the predicate that produced them (round 2's C1(R2)). Without this,
        // rotating a dimensioned edge 90 degrees about its start point left
        // every magnitude unchanged, the plan validated, and the solver
        // silently rewrote the user's vertical line back to horizontal.
        const auto lineFor = [&](SketchEntityId id) -> std::optional<LineRef> {
            const SketchEntity* entity = sketch->findEntity(id);
            if (entity == nullptr) return std::nullopt;
            const auto* line = std::get_if<SketchLine>(&entity->geometry);
            if (line == nullptr) return std::nullopt;
            return LineRef{id, line->start, line->end};
        };
        const auto pointFor = [&](const SketchElementRef& ref) -> std::optional<Vec2> {
            const SketchEntity* entity = sketch->findEntity(ref.entityId);
            if (entity == nullptr) return std::nullopt;
            switch (ref.subElement) {
                case SketchSubElement::StartPoint: return StartPointOf(entity->geometry);
                case SketchSubElement::EndPoint: return EndPointOf(entity->geometry);
                case SketchSubElement::CenterPoint: return MidPointOf(entity->geometry);
                default: return std::nullopt;
            }
        };

        if (const auto* horizontal = std::get_if<HorizontalConstraint>(&planned.data)) {
            const std::optional<LineRef> line = lineFor(horizontal->line);
            if (!line || !IsHorizontal(*line, options.axisAngleToleranceRad))
                return {false, "the plan calls a line horizontal that is no longer horizontal; "
                               "this plan no longer describes this document"};
            continue;
        }
        if (const auto* vertical = std::get_if<VerticalConstraint>(&planned.data)) {
            const std::optional<LineRef> line = lineFor(vertical->line);
            if (!line || !IsVertical(*line, options.axisAngleToleranceRad))
                return {false, "the plan calls a line vertical that is no longer vertical; "
                               "this plan no longer describes this document"};
            continue;
        }
        if (const auto* coincident = std::get_if<CoincidentConstraint>(&planned.data)) {
            const std::optional<Vec2> a = pointFor(coincident->a);
            const std::optional<Vec2> b = pointFor(coincident->b);
            if (!a || !b || !Near(*a, *b, options.coincidenceToleranceMm))
                return {false, "the plan calls two points coincident that have since moved "
                               "apart; this plan no longer describes this document"};
            continue;
        }

        if (!IsDimensional(planned.data)) continue;
        const PlannedParameter* bound = plan.find(planned.parameter);
        if (bound == nullptr) continue; // ValidatePlan already refuses this

        // What the geometry currently measures for this constraint's kind.
        double measured = -1.0;
        if (const auto* length = std::get_if<LengthConstraint>(&planned.data)) {
            const SketchEntity* entity = sketch->findEntity(length->line);
            if (const auto* line = std::get_if<SketchLine>(&entity->geometry))
                measured = std::hypot(line->end.x - line->start.x, line->end.y - line->start.y);
        } else if (const auto* radius = std::get_if<RadiusConstraint>(&planned.data)) {
            const SketchEntity* entity = sketch->findEntity(radius->curve);
            if (const auto* circle = std::get_if<SketchCircle>(&entity->geometry))
                measured = circle->radiusMm;
            else if (const auto* arc = std::get_if<SketchArc>(&entity->geometry))
                measured = arc->radiusMm;
        } else if (const auto* diameter = std::get_if<DiameterConstraint>(&planned.data)) {
            const SketchEntity* entity = sketch->findEntity(diameter->curve);
            if (const auto* circle = std::get_if<SketchCircle>(&entity->geometry))
                measured = 2.0 * circle->radiusMm;
            else if (const auto* arc = std::get_if<SketchArc>(&entity->geometry))
                measured = 2.0 * arc->radiusMm;
        } else {
            continue; // Distance/Angle are not planned by M7
        }

        if (measured < 0.0)
            return {false, std::string("the plan puts a ") + ConstraintKindName(planned.data) +
                               " on geometry of the wrong kind"};

        // The SAME band the analysis believed the source over the drawing with
        // (ADR-M7-009). A plan measured from geometry that has since moved
        // further than that no longer describes this document.
        // The CALLER's band, not the global default (round 2's M4): analysis
        // used options.valueAgreementFraction, so validating with a different
        // constant made a widened tolerance reject the plan it had just
        // produced, blaming an edit that never happened.
        const double reference = std::max(std::abs(bound->value), std::abs(measured));
        if (reference > 0.0 &&
            std::abs(bound->value - measured) / reference > options.valueAgreementFraction)
            return {false, "Parameter '" + bound->name + "' was measured as " +
                               std::to_string(bound->value) +
                               " mm but the geometry it names now measures " +
                               std::to_string(measured) +
                               " mm; this plan no longer describes this document"};
    }
    return {true, {}};
}

ReconstructionResult ApplyReconstruction(PartDocument& document, const ReconstructionPlan& plan,
                                        const ReconstructionOptions& options) {
    ReconstructionResult result;
    result.sketchId = plan.sketchId;
    result.skippedCount = plan.skipped.size();
    // Carried before anything can fail. What the analysis DECLINED to
    // reconstruct is the half of the report that matters most when the rest
    // goes wrong, and a failure path that returns no diagnostics leaves the
    // user with "it did not work" and nothing else (spec 18).
    result.report.sketchId = plan.sketchId;
    result.report.skipped = plan.skipped;

    const PlanValidation validation = ValidatePlan(plan);
    if (!validation) {
        result.message = "reconstruction plan rejected: " + validation.message;
        return result;
    }
    if (plan.documentId != document.id()) {
        // Cheap first pass. It catches two genuinely different documents, and it
        // does NOT catch two loads of one file -- those share this id, which is
        // why the geometric check below is the one that carries the contract.
        result.message = "reconstruction plan was built for a different document";
        return result;
    }
    const PlanValidation describes = ValidatePlanAgainstDocument(document, plan, options);
    if (!describes) {
        result.message = "reconstruction plan rejected: " + describes.message;
        return result;
    }

    // Everything below can fail, and if any of it does the document must end up
    // exactly as it started (spec 16). Undo runs in reverse creation order:
    // constraints release their Parameter graph edges first, then the
    // Parameters themselves come out, so nothing is removed while something
    // still refers to it.
    const auto rollBack = [&document, &result] {
        for (auto it = result.createdConstraints.rbegin(); it != result.createdConstraints.rend();
             ++it)
            document.removeSketchConstraint(result.sketchId, *it);
        for (auto it = result.createdParameters.rbegin(); it != result.createdParameters.rend();
             ++it)
            document.removeObject(*it);
        result.createdConstraints.clear();
        result.createdParameters.clear();
        // The entries name ids this unwind has just destroyed, and
        // FormatReconstructionReport would otherwise tell the user
        // "reconstructed: 10 constraint(s)" about an operation that
        // reconstructed nothing. `skipped` is deliberately NOT cleared: what
        // the analysis declined is exactly the half worth keeping on a failure
        // path (ADR-M7-017).
        result.report.entries.clear();
    };

    std::vector<ObjectId> slotToParameter(plan.parameters.size(), kInvalidObjectId);
    for (std::size_t i = 0; i < plan.parameters.size(); ++i) {
        const PlannedParameter& planned = plan.parameters[i];
        // Through the facade, so registration and the graph node happen the one
        // documented way (spec 5). A Parameter created any other way is one the
        // dependency graph has never heard of.
        const Parameter& created =
            document.addParameter(planned.name, planned.value, planned.unit);
        slotToParameter[i] = created.id();
        result.createdParameters.push_back(created.id());
    }

    for (const PlannedConstraint& planned : plan.constraints) {
        SketchConstraintData data = planned.data;
        if (planned.parameter != kInvalidPlanParameterSlot) {
            const ObjectId parameterId =
                slotToParameter[static_cast<std::size_t>(planned.parameter)];
            // Fill the binding the plan deliberately left empty. Visiting the
            // variant rather than testing for LengthConstraint means adding a
            // tenth dimensional kind cannot silently produce an unbound
            // constraint here (ADR-M3-007's rule).
            std::visit(
                [parameterId](auto& constraint) {
                    using T = std::decay_t<decltype(constraint)>;
                    if constexpr (std::is_same_v<T, DistanceConstraint> ||
                                  std::is_same_v<T, LengthConstraint> ||
                                  std::is_same_v<T, RadiusConstraint> ||
                                  std::is_same_v<T, DiameterConstraint> ||
                                  std::is_same_v<T, AngleConstraint>)
                        constraint.parameterId = parameterId;
                },
                data);
        }

        // addSketchConstraint, not Sketch::addConstraint, so the Parameter ->
        // Sketch graph edge is wired. Without it a Width edit would change a
        // Parameter that nothing was listening to.
        const SketchConstraintId id = document.addSketchConstraint(plan.sketchId, data);
        if (id == kInvalidSketchConstraintId) {
            result.message = std::string("the sketch refused a reconstructed ") +
                             ConstraintKindName(data) + " constraint";
            rollBack();
            return result;
        }
        result.createdConstraints.push_back(id);

        // Provenance, recorded HERE because this is the last moment the answers
        // exist. One line later the constraint is an ordinary native object and
        // nothing distinguishes what the source stated from what M7 chose.
        ProvenanceEntry entry;
        entry.constraintId = id;
        entry.constraintKind = ConstraintKindName(data);
        entry.origin = planned.origin;
        entry.sourceRef = planned.sourceRef;
        entry.targets = ReferencedEntities(data);
        entry.parameterId = BoundParameterId(data);
        if (entry.parameterId != kInvalidObjectId) {
            const Parameter* bound = document.parameters().findById(entry.parameterId);
            if (bound != nullptr) entry.parameterName = bound->name();
        }
        result.report.entries.push_back(std::move(entry));
    }

    result.ok = true;
    result.message = "reconstructed " + std::to_string(result.createdParameters.size()) +
                     " parameter(s) and " + std::to_string(result.createdConstraints.size()) +
                     " constraint(s)";
    if (result.skippedCount > 0)
        result.message += ", skipped " + std::to_string(result.skippedCount);
    return result;
}

const ProvenanceEntry* ReconstructionReport::find(SketchConstraintId id) const noexcept {
    for (const ProvenanceEntry& entry : entries)
        if (entry.constraintId == id) return &entry;
    return nullptr;
}

std::size_t ReconstructionReport::countByOrigin(ReconstructionOrigin origin) const noexcept {
    std::size_t count = 0;
    for (const ProvenanceEntry& entry : entries)
        if (entry.origin == origin) ++count;
    return count;
}

std::string FormatReconstructionReport(const ReconstructionReport& report) {
    std::string text = "Reconstruction report\n";
    text += "  reconstructed: " + std::to_string(report.entries.size()) + " constraint(s)\n";
    text += "    explicit in source: " +
            std::to_string(report.countByOrigin(ReconstructionOrigin::ExplicitSource)) + "\n";
    text += "    inferred from geometry: " +
            std::to_string(report.countByOrigin(ReconstructionOrigin::InferredGeometric)) + "\n";
    text += "    inferred placement: " +
            std::to_string(report.countByOrigin(ReconstructionOrigin::InferredPlacement)) + "\n";

    for (const ProvenanceEntry& entry : report.entries) {
        text += "  - " + entry.constraintKind + " [" + ReconstructionOriginName(entry.origin) + "]";
        if (!entry.parameterName.empty()) text += " controlled by " + entry.parameterName;
        if (!entry.sourceRef.empty()) text += " from " + entry.sourceRef;
        text += "\n";
    }

    // Always emitted, even when empty. A report that omits the section when
    // nothing was skipped cannot be told from one where the question was never
    // asked -- and "nothing was skipped" is exactly the claim a user needs to
    // be able to trust before they rely on the result.
    text += "  skipped: " + std::to_string(report.skipped.size()) + "\n";
    for (const ReconstructionSkip& skip : report.skipped) {
        text += "  - [" + std::string(ReconstructionSkipReasonName(skip.reason)) + "] " +
                skip.detail;
        if (!skip.sourceRef.empty()) text += " (source " + skip.sourceRef + ")";
        text += "\n";
    }
    return text;
}

bool SketchAlreadyReconstructed(const PartDocument& document, ObjectId sketchId) {
    const Sketch* sketch = document.findSketch(sketchId);
    if (sketch == nullptr) return false;
    for (const SketchConstraint& constraint : sketch->constraints()) {
        if (IsDimensional(constraint.data) &&
            BoundParameterId(constraint.data) != kInvalidObjectId)
            return true;
        // A drawing with NO dimensions -- the common case -- still reconstructs
        // a Fix, and exactly one Fix per sketch is the placement policy
        // (ADR-M7-013). Keying only on dimensional constraints let such a
        // sketch be reconstructed repeatedly: 9 constraints became 18 became
        // 27, each run reporting success, with a duplicate Fix and duplicate
        // Coincidents every time.
        //
        // Spec 25 forbids duplicating "equivalent Parameters AND constraints",
        // and the damage here is the worst shape there is (ADR-M7-012): the
        // redundancy is masked while any freedom remains, and only surfaces as
        // OverConstrained once the USER finishes dimensioning -- pointing at
        // the dimension they just added rather than at the double import.
        if (std::holds_alternative<FixConstraint>(constraint.data)) return true;
    }
    // ANY constraint at all, as the final answer (round 2's M3). The two
    // clauses above are proxies for "M7 has run here", and every proxy this
    // predicate has used could be removed by a public ReconstructionOptions
    // flag: with `placeFix = false` there is no Fix to find, and a drawing
    // with no dimensions has nothing dimensional either, so 8 constraints
    // became 16 became 24 with each run reporting success -- ADR-M7-012's
    // deferred damage, restored through a supported option.
    //
    // The widened rule costs one thing and it is worth naming: a sketch a
    // USER constrained by hand can no longer be reconstructed from an
    // imported drawing. That is the correct trade today -- M7.1's contract is
    // refuse-not-merge, and merging into hand-authored constraints is exactly
    // the decision a later slice has to make deliberately rather than
    // inherit. The diagnostic says so.
    return !sketch->constraints().empty();
}

ReconstructionResult ReconstructSketch(PartDocument& document, ObjectId sketchId,
                                       const std::vector<ImportedDimension2D>& dimensions,
                                       const ReconstructionOptions& options) {
    ReconstructionResult result;
    result.sketchId = sketchId;

    // Spec 25: re-running must not silently duplicate. Refusing is the M7.1
    // contract -- a replacement mode is a later slice's decision, and the one
    // thing that must not happen meanwhile is Width, Width_2, Width_3.
    if (SketchAlreadyReconstructed(document, sketchId)) {
        result.message = "this sketch already carries constraints; remove them first to "
                         "reconstruct again (M7 refuses rather than merges, so nothing "
                         "already in the sketch can be duplicated or contradicted)";
        return result;
    }

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketchId, dimensions, options);
    return ApplyReconstruction(document, plan, options);
}

} // namespace paramcad
