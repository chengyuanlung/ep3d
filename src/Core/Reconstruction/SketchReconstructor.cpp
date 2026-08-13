#include "Core/Reconstruction/SketchReconstructor.h"

#include "Core/Document/PartDocument.h"
#include "Core/Parameter/ParameterManager.h"
#include "Core/Sketch/Sketch.h"
#include <algorithm>
#include <cmath>
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

// A corner: two lines meeting at a point, named by which end of each meets.
struct Corner {
    SketchElementRef a{};
    SketchElementRef b{};
    Vec2 at{};
};

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

// --- The rectangle rule (ADR-M7-010) ---------------------------------------
//
// Four lines, each joined end-to-end to exactly two others, forming ONE closed
// loop, every side within the axis tolerance of horizontal or vertical, two of
// each. Anything else is not this shape and gets no constraints from this rule.
struct Rectangle {
    std::vector<Corner> corners;                 // 4
    std::vector<SketchEntityId> horizontalLines; // 2
    std::vector<SketchEntityId> verticalLines;   // 2
};

std::optional<Rectangle> RecognizeRectangle(const std::vector<LineRef>& lines,
                                            const ReconstructionOptions& options) {
    if (lines.size() != 4) return std::nullopt;

    Rectangle rectangle;
    for (const LineRef& line : lines) {
        const bool horizontal = IsHorizontal(line, options.axisAngleToleranceRad);
        const bool vertical = IsVertical(line, options.axisAngleToleranceRad);
        // A line that is both is degenerate (shorter than the tolerance can
        // resolve); a line that is neither is a slope, and neither belongs to
        // the shape this rule recognises.
        if (horizontal == vertical) return std::nullopt;
        if (horizontal) rectangle.horizontalLines.push_back(line.id);
        else rectangle.verticalLines.push_back(line.id);
    }
    if (rectangle.horizontalLines.size() != 2 || rectangle.verticalLines.size() != 2)
        return std::nullopt;

    // Cluster the eight endpoints by location. A closed quadrilateral gives
    // exactly four clusters of exactly two endpoints, and the two in a cluster
    // belong to different lines.
    //
    // Clustering rather than pairwise matching because the shapes that must be
    // REJECTED are the ones a pairwise check accepts: three lines meeting at a
    // point makes a cluster of three, two coincident segments make a cluster of
    // four, and a figure of eight makes two clusters of four. Each of those has
    // every endpoint "matched" and none of them is a rectangle.
    struct Cluster {
        Vec2 at{};
        std::vector<SketchElementRef> members;
    };
    std::vector<Cluster> clusters;
    const SketchSubElement ends[2]{SketchSubElement::StartPoint, SketchSubElement::EndPoint};
    for (const LineRef& line : lines) {
        for (SketchSubElement end : ends) {
            const Vec2 point = line.point(end);
            Cluster* found = nullptr;
            for (Cluster& cluster : clusters)
                if (Near(cluster.at, point, options.coincidenceToleranceMm)) found = &cluster;
            if (found == nullptr) {
                clusters.push_back(Cluster{point, {}});
                found = &clusters.back();
            }
            found->members.push_back(SketchElementRef{line.id, end});
        }
    }

    if (clusters.size() != 4) return std::nullopt;
    for (const Cluster& cluster : clusters) {
        if (cluster.members.size() != 2) return std::nullopt;
        if (cluster.members[0].entityId == cluster.members[1].entityId) return std::nullopt;
        rectangle.corners.push_back(Corner{cluster.members[0], cluster.members[1], cluster.at});
    }

    // Deterministic corner order, by location. Nothing downstream may depend on
    // the order lines were stored in (ADR-M6-004 is the same rule for entities).
    std::sort(rectangle.corners.begin(), rectangle.corners.end(),
              [](const Corner& a, const Corner& b) { return Before(a.at, b.at); });
    std::sort(rectangle.horizontalLines.begin(), rectangle.horizontalLines.end());
    std::sort(rectangle.verticalLines.begin(), rectangle.verticalLines.end());
    return rectangle;
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
    plan.sketchId = sketchId;

    const std::vector<LineRef> lines = CollectLines(*sketch);
    const std::optional<Rectangle> rectangle =
        options.recognizeRectangle ? RecognizeRectangle(lines, options) : std::nullopt;

    // --- Geometric relations (spec 8, Class C) ------------------------------
    if (rectangle.has_value()) {
        if (options.recognizeCoincident) {
            for (const Corner& corner : rectangle->corners)
                plan.constraints.push_back(
                    PlannedConstraint{CoincidentConstraint{corner.a, corner.b},
                                      kInvalidPlanParameterSlot,
                                      ReconstructionOrigin::InferredGeometric,
                                      "rectangle corner"});
        }
        if (options.recognizeHorizontal) {
            for (SketchEntityId line : rectangle->horizontalLines)
                plan.constraints.push_back(PlannedConstraint{
                    HorizontalConstraint{line}, kInvalidPlanParameterSlot,
                    ReconstructionOrigin::InferredGeometric, "rectangle side within axis tolerance"});
        }
        if (options.recognizeVertical) {
            for (SketchEntityId line : rectangle->verticalLines)
                plan.constraints.push_back(PlannedConstraint{
                    VerticalConstraint{line}, kInvalidPlanParameterSlot,
                    ReconstructionOrigin::InferredGeometric, "rectangle side within axis tolerance"});
        }
        if (options.placeFix && !rectangle->corners.empty()) {
            // The lexicographically smallest corner, so the choice is a fact
            // about the geometry and not about the order it was read in
            // (ADR-M7-008). Without a Fix the sketch keeps its two global
            // translation freedoms and can never reach DOF 0, however many
            // dimensions are reconstructed.
            plan.constraints.push_back(PlannedConstraint{
                FixConstraint{rectangle->corners.front().a}, kInvalidPlanParameterSlot,
                ReconstructionOrigin::InferredPlacement, "deterministic placement"});
        }
    }

    if (!options.reconstructExplicitDimensions) return plan;

    // --- Explicit source dimensions (spec 8, Class A / Class B) -------------
    std::vector<Candidate> resolved;
    for (const ImportedDimension2D& dimension : dimensions) {
        const std::string what = Describe(dimension);

        if (dimension.kind != ImportedDimensionKind::Linear &&
            dimension.kind != ImportedDimensionKind::Aligned) {
            plan.skipped.push_back(
                ReconstructionSkip{ReconstructionSkipReason::UnsupportedKind,
                                   what + " is not reconstructed in M7.1", dimension.sourceHandle});
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
        resolved.push_back(Candidate{&dimension, value, target.id,
                                     IsHorizontal(target, options.axisAngleToleranceRad),
                                     Before(target.start, target.end) ? target.start : target.end});
    }

    // Deterministic naming order, by geometry: horizontal dimensions first,
    // then by location. Naming must not depend on the order dimensions appeared
    // in the file, or the same drawing exported twice would produce Width and
    // Height the other way round.
    std::sort(resolved.begin(), resolved.end(), [](const Candidate& a, const Candidate& b) {
        if (a.targetIsHorizontal != b.targetIsHorizontal) return a.targetIsHorizontal;
        return Before(a.sortKey, b.sortKey);
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

    return plan;
}

ReconstructionResult ApplyReconstruction(PartDocument& document, const ReconstructionPlan& plan) {
    ReconstructionResult result;
    result.sketchId = plan.sketchId;
    result.skippedCount = plan.skipped.size();

    const PlanValidation validation = ValidatePlan(plan);
    if (!validation) {
        result.message = "reconstruction plan rejected: " + validation.message;
        return result;
    }
    if (document.findSketch(plan.sketchId) == nullptr) {
        result.message = "reconstruction plan names a sketch this document does not have";
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
    }

    result.ok = true;
    result.message = "reconstructed " + std::to_string(result.createdParameters.size()) +
                     " parameter(s) and " + std::to_string(result.createdConstraints.size()) +
                     " constraint(s)";
    if (result.skippedCount > 0)
        result.message += ", skipped " + std::to_string(result.skippedCount);
    return result;
}

bool SketchAlreadyReconstructed(const PartDocument& document, ObjectId sketchId) {
    const Sketch* sketch = document.findSketch(sketchId);
    if (sketch == nullptr) return false;
    for (const SketchConstraint& constraint : sketch->constraints())
        if (IsDimensional(constraint.data) &&
            BoundParameterId(constraint.data) != kInvalidObjectId)
            return true;
    return false;
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
        result.message = "this sketch already carries reconstructed dimensional constraints; "
                         "remove them first to reconstruct again";
        return result;
    }

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketchId, dimensions, options);
    return ApplyReconstruction(document, plan);
}

} // namespace paramcad
