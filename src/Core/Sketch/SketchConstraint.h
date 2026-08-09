#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Sketch/SketchTypes.h"
#include <variant>
#include <vector>

namespace paramcad {

// Stable identity for one constraint inside a Sketch (ADR-M5-001).
//
// A distinct type allocated from the shared ObjectIdGenerator, exactly as
// SketchEntityId is (ADR-M4-001): distinct so the compiler rejects passing an
// entity id where a constraint id belongs, and drawn from the shared generator
// so it inherits M1's restore-collision safety rather than a private counter.
//
// Full identity is the pair (Sketch::id(), SketchConstraintId).
enum class SketchConstraintId : ObjectId {};

constexpr SketchConstraintId kInvalidSketchConstraintId{kInvalidObjectId};

inline ObjectId ToObjectId(SketchConstraintId id) noexcept {
    return static_cast<ObjectId>(id);
}

inline SketchConstraintId NextSketchConstraintId() {
    return static_cast<SketchConstraintId>(ObjectIdGenerator::Next());
}

inline SketchConstraintId RestoreSketchConstraintId(SketchConstraintId id) {
    ObjectIdGenerator::AdvancePast(ToObjectId(id));
    return id;
}

// --- Constraint kinds, as distinct types -----------------------------------
//
// A variant of purpose-built structs rather than one struct with a kind tag and
// unused reference slots (spec 6 prefers type-safe semantic objects over an
// unvalidated property bag). Each carries exactly the references it needs, so a
// Horizontal constraint with two targets, or a Length constraint with no
// parameter, cannot be constructed at all -- whole classes of invalid
// cardinality stop being runtime validation problems.

// Two points (entity endpoints or centres) occupy the same location.
struct CoincidentConstraint {
    SketchElementRef a{};
    SketchElementRef b{};
};

// A line's endpoints share the same v (Horizontal) or u (Vertical) coordinate.
struct HorizontalConstraint {
    SketchEntityId line{kInvalidSketchEntityId};
};

struct VerticalConstraint {
    SketchEntityId line{kInvalidSketchEntityId};
};

// A point is pinned where it currently is. This is what removes the global
// translation freedom that would otherwise leave every sketch under-constrained
// (ADR-M5-005).
struct FixConstraint {
    SketchElementRef target{};
};

// --- Dimensional constraints ------------------------------------------------
// Each binds to an existing Parameter by ObjectId (ADR-M5-002). There is no
// second scalar system: the parameter participates in the M2 dependency graph
// as an ordinary prerequisite, which is what makes Width 100 -> 120 propagate
// through existing machinery rather than through anything new.

// Point-to-point distance, in mm.
struct DistanceConstraint {
    SketchElementRef a{};
    SketchElementRef b{};
    ObjectId parameterId{kInvalidObjectId};
};

// A line's length, in mm.
struct LengthConstraint {
    SketchEntityId line{kInvalidSketchEntityId};
    ObjectId parameterId{kInvalidObjectId};
};

// A circle's or arc's radius, in mm.
struct RadiusConstraint {
    SketchEntityId curve{kInvalidSketchEntityId};
    ObjectId parameterId{kInvalidObjectId};
};

// A circle's or arc's diameter, in mm. NOT separate state: it drives the same
// underlying radius as radius = diameter / 2, so a Radius and a Diameter
// constraint on the same curve are two views of one quantity and conflict if
// they disagree (spec 11, ADR-M5-002).
struct DiameterConstraint {
    SketchEntityId curve{kInvalidSketchEntityId};
    ObjectId parameterId{kInvalidObjectId};
};

// Angle FROM lineA TO lineB, counter-clockwise, in radians (ADR-M5-006). Each
// line's direction is its stored start -> end, so reversing how a line was
// drawn changes the measured angle by pi -- deliberate, because an undirected
// angle cannot distinguish a 60-degree corner from a 120-degree one.
struct AngleConstraint {
    SketchEntityId lineA{kInvalidSketchEntityId};
    SketchEntityId lineB{kInvalidSketchEntityId};
    ObjectId parameterId{kInvalidObjectId};
};

using SketchConstraintData =
    std::variant<CoincidentConstraint, HorizontalConstraint, VerticalConstraint,
                 FixConstraint, DistanceConstraint, LengthConstraint, RadiusConstraint,
                 DiameterConstraint, AngleConstraint>;

struct SketchConstraint {
    SketchConstraintId id{kInvalidSketchConstraintId};
    SketchConstraintData data{};
};

// Human-facing kind name, used by the serializer for type dispatch and by the
// UI for the constraint list. Keyed by the variant alternative, never by a
// stored tag that could disagree with the payload.
const char* ConstraintKindName(const SketchConstraintData& data) noexcept;

// Every entity this constraint refers to. Lets a caller check membership,
// find dangling references, or cascade a deletion without enumerating
// constraint kinds at its own call site (ADR-M3-007's rule again). It was
// file-local in Sketch.cpp until persistence and the deletion policy needed the
// same answer -- three copies of this switch would be three places to forget a
// kind when the tenth constraint type is added.
std::vector<SketchEntityId> ReferencedEntities(const SketchConstraintData& data);

// Whether this kind READS a Parameter at all. Distinct from
// `BoundParameterId(data) != kInvalidObjectId`, which cannot tell "this kind
// needs no parameter" from "this kind needs one and has none" -- a distinction
// three separate call sites got wrong in the same way.
bool IsDimensional(const SketchConstraintData& data) noexcept;

// The Parameter this constraint reads, or kInvalidObjectId for the
// non-dimensional kinds. Lets callers collect parameter dependencies without
// enumerating constraint types (the capability-over-type-list rule of
// ADR-M3-007, applied here).
ObjectId BoundParameterId(const SketchConstraintData& data) noexcept;

// Smallest dimensional value a constraint may carry, in mm. Zero is invalid for
// lengths, distances and radii: a zero-length line has no direction and a
// zero-radius circle is a point (ADR-M5-002).
//
// It must sit ABOVE `kSketchToleranceMm`, not on it. Both were 1e-6, and the
// two bounds are inclusive from opposite sides -- `DimensionValueValid` accepts
// `value >= 1e-6` while `SamePoint` calls two points identical at
// `<= 1e-6`. The smallest ACCEPTED dimension was therefore the largest REJECTED
// geometry: a Length of exactly 1e-6 was a legal dimension whose solved line
// was, by definition, degenerate. The solver's own absolute residual tolerance
// (1e-9) widened that dead band further, and 41 of 144 swept configurations had
// a converged solve refused at commit.
//
// Ten times the coincidence tolerance leaves room for the residual tolerance
// and for ordinary floating-point settling. 1e-5 mm is 10 nanometres -- far
// below anything a CAD user models, so nothing real is excluded.
inline constexpr double kMinSketchDimensionMm = 1e-5;

// The gap is the point, so it is asserted at COMPILE TIME rather than left to a
// behavioural test that only fails for some starting configurations. A test
// that reverted the constant still passed, because whether a solve at the floor
// lands above or below the coincidence tolerance depends on where it started --
// 41 of 144 swept configurations failed, and the test's one configuration was
// among the other 103.
static_assert(kMinSketchDimensionMm > 10.0 * kSketchToleranceMm,
              "the smallest accepted dimension must sit clear of the coincidence "
              "tolerance, or a legal dimension produces geometry the sketch calls "
              "degenerate");

} // namespace paramcad
