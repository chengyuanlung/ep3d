#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Sketch/SketchTypes.h"
#include <variant>
#include <optional>
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

// THE SAME TWO EQUATIONS, said about two POINTS that no line joins (M26.3).
//
// `a.v == b.v` is horizontal alignment and `a.u == b.u` is vertical alignment,
// which is exactly what the two above mean about a line's own endpoints -- so
// the solver reuses PointsEqualV/PointsEqualU rather than growing a residual,
// and there is only ever one formula for "these two share an axis".
//
// A SEPARATE TYPE rather than an optional second reference bolted onto
// HorizontalConstraint, for the reason stated at the top of this section: a
// struct with slots that are meaningless half the time is the property bag
// this file refuses, and "line is set XOR a and b are set" would be an
// invariant nothing enforces.
//
// The two forms are NOT interchangeable spellings. Selecting a line's two
// endpoints and asking for Horizontal produces the LINE form, normalised once
// where the command is built -- see requestConstraint. Storing that as a point
// pair would be a second way to say one thing, and this project has paid for
// that shape before.
struct PointsHorizontalConstraint {
    SketchElementRef a{};
    SketchElementRef b{};
};

struct PointsVerticalConstraint {
    SketchElementRef a{};
    SketchElementRef b{};
};

// A point is pinned where it currently is. This is what removes the global
// translation freedom that would otherwise leave every sketch under-constrained
// (ADR-M5-005).
struct FixConstraint {
    SketchElementRef target{};
};

// --- Geometric constraints added in M13 (roadmap 6.1's "next stage") --------
//
// All seven are NON-dimensional: they bind no Parameter and carry no value.
// That is what separates them from 7's dimensions -- they say what the
// RELATIONSHIP is, not what the number is.

// Two lines have the same direction (up to sign). Zero when the normalised
// cross product of their directions is zero, so 0 degrees and 180 degrees are
// both parallel -- which is what "parallel" means and what distinguishes this
// from an Angle of exactly 0.
struct ParallelConstraint {
    SketchEntityId lineA{kInvalidSketchEntityId};
    SketchEntityId lineB{kInvalidSketchEntityId};
};

struct PerpendicularConstraint {
    SketchEntityId lineA{kInvalidSketchEntityId};
    SketchEntityId lineB{kInvalidSketchEntityId};
};

// Two lines have equal LENGTH, or two circles/arcs have equal RADIUS.
//
// ONE constraint type for both, resolved by what the entities actually are,
// because the user's intent -- "these two are the same size" -- is one idea.
// Mixing a line with a circle is refused when the problem is built: there is no
// meaningful equality between a length and a radius.
struct EqualConstraint {
    SketchEntityId a{kInvalidSketchEntityId};
    SketchEntityId b{kInvalidSketchEntityId};
};

// Two circles/arcs share a centre.
//
// Deliberately NOT a pair of SketchElementRef. A point coinciding with a
// curve's centre is already expressible -- and already implemented -- as
// Coincident(point, curve.CenterPoint); a second spelling of the same
// relationship would be two constraint types the solver cannot tell apart.
// What Concentric adds is the curve-to-curve case as a NAMED idea, so the
// constraint list says "Concentric" where the user meant concentric.
struct ConcentricConstraint {
    SketchEntityId curveA{kInvalidSketchEntityId};
    SketchEntityId curveB{kInvalidSketchEntityId};
};

// A point sits at the midpoint of a line.
struct MidpointConstraint {
    SketchElementRef point{};
    SketchEntityId line{kInvalidSketchEntityId};
};

// A point lies ON another entity: anywhere along a line, or anywhere on a
// circle's or arc's rim.
//
// For a line this is the INFINITE line through its endpoints, not the segment.
// Constraining a point to stay between the endpoints as well would need an
// inequality, and this solver has only equalities -- claiming otherwise would
// be a constraint that silently does half of what its name says.
struct PointOnObjectConstraint {
    SketchElementRef point{};
    SketchEntityId target{kInvalidSketchEntityId};
};

// Two entities touch at exactly one point: a line and a curve, or two curves.
//
// `internal` applies ONLY to the curve-curve case and distinguishes the two
// genuinely different configurations -- one circle outside the other (centre
// distance = r1 + r2) from one inside the other (centre distance = |r1 - r2|).
//
// It is STORED, not re-derived from the geometry at each solve. Re-deriving it
// would make the constraint mean whatever the current configuration happens to
// suggest, so dragging a circle through its neighbour would silently rewrite
// what the user asked for. The UI picks the branch once, from the configuration
// at creation time, and the choice is then a property of the constraint.
//
// `at` names WHICH END OF `a` the two touch at, when that is already known --
// and it changes which equation the solver is given, because the usual ones
// stop working there.
//
//   * line and curve: "the centre is `r` from the infinite line" is true and
//     useless once a coincidence has pinned an end of the line onto the curve.
//     That distance can no longer EXCEED `r`, so the residual sits at a maximum
//     with a vanishing gradient and holds nothing.
//   * two curves: same disease. Two circles sharing a point already obey
//     |r1 - r2| <= |C1 - C2| <= r1 + r2, so the outer form sits at a maximum
//     and the inner one at a minimum, and neither holds anything either.
//
// At a KNOWN point both become statements about an ANGLE, which does hold:
// perpendicularity for the line (TangentAtPoint) and collinear radii for the
// pair (TangentCurvesAtPoint).
//
// `a` MUST be the entity that owns the named end -- the line in a line-curve
// pair, and an arc (never a circle, which has no ends) in a curve-curve one.
// Every tool that asks for a pinned tangency orders the pair that way.
//
// `internal` is IGNORED when `at` is set on a curve pair: collinear covers both
// configurations, and which one a sketch is in is settled by where it already
// is rather than by a flag. The flag matters for the un-pinned form precisely
// because that residual cannot see the touch point.
//
// `Whole` means "not known", which is what a tangency between a free line and a
// circle is -- and what every constraint written before M17.21 meant. STORED,
// like `internal` and for the same reason: deriving it from whichever
// coincidences happen to exist would let deleting an unrelated constraint
// silently change what this one says.
struct TangentConstraint {
    SketchEntityId a{kInvalidSketchEntityId};
    SketchEntityId b{kInvalidSketchEntityId};
    bool internal{false};
    SketchSubElement at{SketchSubElement::Whole};
};

// Two points are mirror images of each other across a line.
//
// NON-dimensional: it binds no Parameter and carries no value, like the rest of
// the M13 family. What it says is a relationship -- "these two are the same
// distance from that line, on opposite sides, square to it" -- and the distance
// itself remains whatever the rest of the sketch makes it.
//
// TWO POINTS, not two entities. Mirroring a line means its two ENDS are each
// symmetric to the copy's corresponding end, and saying that as a pair of
// point constraints is both simpler and more general: it also expresses a
// circle's centre, a lone point, and one end of a line whose other end the user
// wants free. A whole-entity form would be a second spelling of the same idea
// (the argument ConcentricConstraint records).
//
// The mirror is a LINE ENTITY rather than a stored axis, so moving the mirror
// moves everything mirrored across it. A stored axis would be a copy of the
// line that stops agreeing with it the moment either changes.
struct SymmetricConstraint {
    SketchElementRef a{};
    SketchElementRef b{};
    SketchEntityId line{kInvalidSketchEntityId};
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

// The HORIZONTAL and VERTICAL separation of two points, in mm: the two legs of
// the right triangle whose hypotenuse DistanceConstraint measures.
//
// Separate types rather than a Distance with an axis flag, for the reason the
// whole variant exists: each carries exactly what it needs, and a caller that
// handles one and forgets the other fails to compile rather than at runtime.
//
// SIGNED, and this is the load-bearing decision. The residual is
// `(b.u - a.u) - target`, not `|b.u - a.u| - target`:
//
//  * The absolute form has no derivative at zero, which is exactly where a
//    solver lands when two points are vertically aligned and it is asked for a
//    horizontal separation. The signed form is smooth everywhere.
//  * The UI orders the pair so the seeded value is POSITIVE, so a drawing shows
//    a positive number the way a drawing should.
//  * Typing a negative value therefore means something definite -- b moves to
//    the other side of a -- rather than being refused or, worse, silently
//    solving to the mirror image of what was typed.
struct HorizontalDistanceConstraint {
    SketchElementRef a{};
    SketchElementRef b{};
    ObjectId parameterId{kInvalidObjectId};
};

struct VerticalDistanceConstraint {
    SketchElementRef a{};
    SketchElementRef b{};
    ObjectId parameterId{kInvalidObjectId};
};

// The PERPENDICULAR distance from a point to a line, in mm.
//
// The dimensional twin of PointOnObjectConstraint: same residual, a target
// instead of zero. That is what makes it cheap and what makes it trustworthy --
// the two agree about which side of the line is positive because they compute
// the same number.
//
// SIGNED, like the axis distances and for the same reason: the absolute form
// has no derivative where the point sits ON the line, which is exactly the
// configuration an offset of zero passes through. The command that creates one
// orders it so the value starts positive.
//
// Against the INFINITE line through the segment, not the segment itself. A
// point beyond the end of a line still has a well-defined distance to it, and
// the alternative -- clamping to the nearest endpoint -- is a different
// quantity with a kink at each end.
struct PointLineDistanceConstraint {
    SketchElementRef point{};
    SketchEntityId line{kInvalidSketchEntityId};
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

// One of an ELLIPSE's two semi-axes, in mm (M17.25).
//
// A separate kind from RadiusConstraint, and `minor` is stored rather than
// inferred from which of the two is currently longer. Both matter:
//
//   * An ellipse has TWO radii. A RadiusConstraint on one would have to pick,
//     and whichever it picked would be a silent choice the constraint list
//     could not show. The solve session refuses Radius and Diameter on an
//     ellipse and says to use this instead.
//   * Deriving `minor` from the geometry would let a resize that made the minor
//     axis the longer one silently swap which axis the dimension drives -- and
//     the ellipse would then rotate a quarter turn to satisfy a number the user
//     had not touched. It is a property of the constraint, decided once.
struct EllipseAxisConstraint {
    SketchEntityId curve{kInvalidSketchEntityId};
    ObjectId parameterId{kInvalidObjectId};
    bool minor{false};
};

// WHERE AN ELLIPSE'S MAJOR AXIS POINTS, in radians from the sketch's +u
// (M17.25).
//
// The third and last of an ellipse's own dimensions, and without it an ellipse
// can never be fully constrained: its centre, its two semi-axes and its
// orientation are five numbers, and only four of them had a dimension. A shape
// that always reports a leftover degree of freedom is one the DOF readout can
// never call finished.
struct EllipseRotationConstraint {
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
                 DiameterConstraint, AngleConstraint,
                 // M13 -- roadmap 6.1's "next stage" geometric constraints.
                 ParallelConstraint, PerpendicularConstraint, EqualConstraint,
                 ConcentricConstraint, MidpointConstraint, PointOnObjectConstraint,
                 TangentConstraint,
                 // M17 -- the two legs of a point-to-point distance (roadmap 7.1).
                 HorizontalDistanceConstraint, VerticalDistanceConstraint,
                 PointLineDistanceConstraint, SymmetricConstraint,
                 // M17.25 -- an ellipse's semi-axes and its orientation.
                 EllipseAxisConstraint, EllipseRotationConstraint,
                 // M26.3 -- horizontal/vertical alignment of two POINTS.
                 PointsHorizontalConstraint, PointsVerticalConstraint>;

struct SketchConstraint {
    SketchConstraintId id{kInvalidSketchConstraintId};
    SketchConstraintData data{};

    // A DRIVEN (reference) dimension: it MEASURES the geometry instead of
    // driving it (M17.19, ADR-M17-042).
    //
    // A FLAG, not a kind, for the same reason `construction` is a flag on an
    // entity: a driven Length is still a Length -- it refers to the same line
    // and shows the same number -- and it differs only in which way the
    // information flows. A parallel hierarchy of driven kinds would double
    // every switch in the solver, the serializer and the panel to express one
    // boolean.
    //
    // What changes: the solver contributes NO residual for it, so it takes no
    // degree of freedom away; and after the solve its bound Parameter is
    // written with what the geometry actually measures. The number is derived,
    // so typing into it is refused -- an editable field that silently reverts
    // on the next recompute is worse than one that says why it will not take
    // the value.
    //
    // Meaningless on a non-dimensional constraint, and refused there rather
    // than ignored: "a driven Horizontal" describes nothing.
    bool driven{false};
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
// EVERY element a constraint names, visited in ONE place.
//
// `visit(entityId, subElement)` is called once per reference. `Data` may be
// const or mutable, and that is the point: reading the list and RETARGETING it
// are the same list of fields, and this project has already paid twice for two
// hand-written copies of one set of facts. Split retargets (a constraint moves
// onto the piece that still owns what it named); everything else reads.
//
// A template in the header rather than two functions in the .cpp, because two
// functions is exactly the shape of the defect.
template <typename Data, typename Visit>
void VisitConstraintElements(Data& data, Visit&& visit) {
    constexpr SketchSubElement kWhole = SketchSubElement::Whole;
    std::visit(
        [&visit](auto& c) {
            using T = std::decay_t<decltype(c)>;
            constexpr SketchSubElement kAll = SketchSubElement::Whole;
            if constexpr (std::is_same_v<T, CoincidentConstraint> ||
                          std::is_same_v<T, DistanceConstraint> ||
                          std::is_same_v<T, HorizontalDistanceConstraint> ||
                          std::is_same_v<T, VerticalDistanceConstraint> ||
                          std::is_same_v<T, PointsHorizontalConstraint> ||
                          std::is_same_v<T, PointsVerticalConstraint>) {
                visit(c.a.entityId, c.a.subElement);
                visit(c.b.entityId, c.b.subElement);
            } else if constexpr (std::is_same_v<T, FixConstraint>) {
                visit(c.target.entityId, c.target.subElement);
            } else if constexpr (std::is_same_v<T, HorizontalConstraint> ||
                                 std::is_same_v<T, VerticalConstraint> ||
                                 std::is_same_v<T, LengthConstraint>) {
                visit(c.line, kAll);
            } else if constexpr (std::is_same_v<T, RadiusConstraint> ||
                                 std::is_same_v<T, DiameterConstraint> ||
                                 std::is_same_v<T, EllipseAxisConstraint> ||
                                 std::is_same_v<T, EllipseRotationConstraint>) {
                visit(c.curve, kAll);
            } else if constexpr (std::is_same_v<T, AngleConstraint> ||
                                 std::is_same_v<T, ParallelConstraint> ||
                                 std::is_same_v<T, PerpendicularConstraint>) {
                visit(c.lineA, kAll);
                visit(c.lineB, kAll);
            } else if constexpr (std::is_same_v<T, EqualConstraint>) {
                visit(c.a, kAll);
                visit(c.b, kAll);
            } else if constexpr (std::is_same_v<T, TangentConstraint>) {
                // `a` IS NAMED AT ITS END when the tangency says where it holds
                // (ADR-M17-045). Reporting Whole there would hide the one fact
                // an operation on that end most needs to know.
                visit(c.a, c.at);
                visit(c.b, kAll);
            } else if constexpr (std::is_same_v<T, ConcentricConstraint>) {
                visit(c.curveA, kAll);
                visit(c.curveB, kAll);
            } else if constexpr (std::is_same_v<T, MidpointConstraint> ||
                                 std::is_same_v<T, PointLineDistanceConstraint>) {
                visit(c.point.entityId, c.point.subElement);
                visit(c.line, kAll);
            } else if constexpr (std::is_same_v<T, SymmetricConstraint>) {
                visit(c.a.entityId, c.a.subElement);
                visit(c.b.entityId, c.b.subElement);
                visit(c.line, kAll);
            } else {
                static_assert(std::is_same_v<T, PointOnObjectConstraint>);
                visit(c.point.entityId, c.point.subElement);
                visit(c.target, kAll);
            }
        },
        data);
    (void)kWhole;
}

// Every ELEMENT a constraint names -- entity AND sub-element.
//
// The primitive, because sub-element is part of what a constraint says: a
// Coincident on a line's end and a Horizontal on the whole line refer to the
// same entity and mean different things, and an operation that reshapes one END
// of a line has to be able to tell them apart. Split is the first caller that
// does; before it, everything only needed the entity.
std::vector<SketchElementRef> ReferencedElements(const SketchConstraintData& data);

// The entities it names. DERIVED from the above, so the two cannot drift: the
// bug this project keeps paying for is two hand-written lists of the same
// facts, and this used to be the second one.
std::vector<SketchEntityId> ReferencedEntities(const SketchConstraintData& data);

// Whether this kind READS a Parameter at all. Distinct from
// `BoundParameterId(data) != kInvalidObjectId`, which cannot tell "this kind
// needs no parameter" from "this kind needs one and has none" -- a distinction
// three separate call sites got wrong in the same way.
bool IsDimensional(const SketchConstraintData& data) noexcept;

// WHAT THE GEOMETRY CURRENTLY MEASURES for a dimensional constraint, in the
// constraint's own unit and sign (M17.19, ADR-M17-042).
//
// THE one measurement site. Every dimensional constraint has a formula that
// the solver's residual drives to zero, and until now that formula existed
// twice: once as the residual, once as the "seed" a new dimension is created
// with so that adding one never moves anything. A driven dimension would have
// needed a third, and three copies of "what is the distance between these two
// points" is three chances to answer differently -- which would show as a
// reference dimension reading a number the driving one does not.
//
// Signed where the constraint is signed (HorizontalDistance, VerticalDistance,
// PointLineDistance, Angle), because the sign is part of what those mean.
//
// nullopt when the constraint is not dimensional, or when its geometry cannot
// be measured -- a reference to an entity that is gone, a sub-element the
// entity does not have, a line too short to have a direction.
std::optional<double> MeasureConstraint(const class Sketch& sketch,
                                        const SketchConstraintData& data);

// The Parameter this constraint reads, or kInvalidObjectId for the
// non-dimensional kinds. Lets callers collect parameter dependencies without
// enumerating constraint types (the capability-over-type-list rule of
// ADR-M3-007, applied here).
ObjectId BoundParameterId(const SketchConstraintData& data) noexcept;

// The same list, as a MUTABLE handle -- so a command that copies a constraint
// can give the copy its own Parameter without a second enumeration of which
// kinds have one. `visit(ObjectId&)` is called once, and only for the kinds
// that read a parameter.
//
// Const or mutable, for the reason VisitConstraintElements gives above: the
// second hand-written copy of a list of fields is where this project's defects
// live.
template <typename Data, typename Visit>
void VisitBoundParameter(Data& data, Visit&& visit) {
    std::visit(
        [&visit](auto& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, DistanceConstraint> ||
                          std::is_same_v<T, LengthConstraint> ||
                          std::is_same_v<T, RadiusConstraint> ||
                          std::is_same_v<T, DiameterConstraint> ||
                          std::is_same_v<T, EllipseAxisConstraint> ||
                          std::is_same_v<T, EllipseRotationConstraint> ||
                          std::is_same_v<T, AngleConstraint> ||
                          std::is_same_v<T, HorizontalDistanceConstraint> ||
                          std::is_same_v<T, VerticalDistanceConstraint> ||
                          std::is_same_v<T, PointLineDistanceConstraint>) {
                visit(c.parameterId);
            }
        },
        data);
}

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
