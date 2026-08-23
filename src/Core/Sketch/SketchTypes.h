#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Geometry/MathTypes.h"
#include <cmath>
#include <cstddef>
#include <optional>
#include <variant>
#include <vector>

namespace paramcad {

// Stable identity for one entity inside a Sketch (ADR-M4-001).
//
// A distinct type, so the compiler rejects passing an ObjectId where an entity
// id belongs -- M4 is the first milestone where the two id spaces coexist and
// could be confused. Values come from the SAME ObjectIdGenerator as ObjectId,
// so entity ids inherit the collision-safety machinery M1 built and M3 relied
// on (AdvancePast on restore, the kMaxObjectId cap, the no-wrap guarantee). A
// private counter would reintroduce exactly the restore-collision bug class
// that machinery exists to prevent.
//
// Full identity of an entity is the pair (Sketch::id(), SketchEntityId).
enum class SketchEntityId : ObjectId {};

constexpr SketchEntityId kInvalidSketchEntityId{kInvalidObjectId};

inline ObjectId ToObjectId(SketchEntityId id) noexcept {
    return static_cast<ObjectId>(id);
}

inline SketchEntityId NextSketchEntityId() {
    return static_cast<SketchEntityId>(ObjectIdGenerator::Next());
}

// Restore counterpart of RestoreObjectId, for deserialized entity ids.
inline SketchEntityId RestoreSketchEntityId(SketchEntityId id) {
    ObjectIdGenerator::AdvancePast(ToObjectId(id));
    return id;
}

// Addressable parts of an entity (ADR-M4-001). RESERVED for M5 constraints;
// M4 constructs these refs for profile orientation but attaches no constraint
// semantics to them. Whole is the only part M4 actually resolves.
enum class SketchSubElement {
    Whole,
    StartPoint,
    EndPoint,
    CenterPoint,
    // ONE OF A SPLINE'S INTERIOR POINTS, named by `SketchElementRef::index`
    // (M17.30).
    //
    // INTERIOR ONLY: index 0 is StartPoint and index n-1 is EndPoint, and a
    // SplinePoint naming either of those is REFUSED rather than accepted as a
    // second spelling. Two ways to say the same point is the shape of defect
    // this project keeps paying for -- the same point would compare unequal to
    // itself, so selecting it twice would look like selecting two things.
    SplinePoint
};

struct SketchElementRef {
    SketchEntityId entityId{kInvalidSketchEntityId};
    SketchSubElement subElement{SketchSubElement::Whole};
    // WHICH interior point, and meaningful ONLY for SplinePoint.
    //
    // Zero for every other sub-element so that two refs to the same thing
    // compare equal whoever built them -- an index left over from a previous
    // use would make a StartPoint unequal to a StartPoint.
    int index{0};
};

// --- Entity geometry, in sketch-local (u,v) millimetres (ADR-M4-002) --------
// Never world XYZ: moving a sketch's plane must not rewrite its geometry.
// Line endpoints own their coordinates rather than referencing shared Point
// entities (ADR-M4-001) -- shared points would make coincidence implicit in the
// storage model, and M4 defers the constraint solver that would maintain it.

struct SketchPoint {
    Vec2 position{};
};

struct SketchLine {
    Vec2 start{};
    Vec2 end{};
};

struct SketchCircle {
    Vec2 center{};
    double radiusMm{0.0};
};

// Angles in radians (spec 7), measured from the sketch +u axis.
// counterClockwise selects which of the two arcs between the angles is meant,
// so the representation is unambiguous without relying on angle ordering.
struct SketchArc {
    Vec2 center{};
    double radiusMm{0.0};
    double startAngleRad{0.0};
    double endAngleRad{0.0};
    bool counterClockwise{true};
};

// --- Ellipses (M17.25) ------------------------------------------------------
//
// A SEPARATE TYPE from SketchCircle, not two extra fields on it.
//
// The tempting move is to give a circle a second radius and a rotation, with a
// circle being the case where they agree -- and it is the wrong one. Every one
// of the ~140 places in this project that reads `circle.radiusMm` would keep
// COMPILING and start quietly meaning "the major radius", which is the silent
// wrong answer this codebase exists to make impossible. A new alternative in
// the variant breaks the exhaustive visits instead, and the compiler names
// every site that has to decide what it wants to do.
//
// Two types, closed and open, mirroring Circle and Arc -- because HasEndpoints
// is the difference that matters to a profile, and it is a property of the
// TYPE there.
struct SketchEllipse {
    Vec2 center{};
    // Major is the LONGER one, and it is not merely a naming convention: the
    // rotation is measured to the major axis, so swapping them silently turns
    // the ellipse a quarter turn. add* enforces major >= minor.
    double majorRadiusMm{0.0};
    double minorRadiusMm{0.0};
    // Where the major axis points, from the sketch's +u, counter-clockwise.
    double rotationRad{0.0};
};

// The open one. Angles are PARAMETRIC, not geometric:
//
//     point(t) = centre + R(rotation) * (major * cos t, minor * sin t)
//
// t is NOT the angle from the centre to the point unless the ellipse happens
// to be a circle. Writing atan2(p - centre) into one of these is a bug that
// looks right at t = 0 and t = pi/2 and is wrong everywhere else, so the two
// are never mixed: PointOnEllipse below is the only way to turn a t into a
// point, and EllipseParamOf the only way back.
struct SketchEllipticalArc {
    Vec2 center{};
    double majorRadiusMm{0.0};
    double minorRadiusMm{0.0};
    double rotationRad{0.0};
    double startParamRad{0.0};
    double endParamRad{0.0};
    bool counterClockwise{true};
};

// --- Splines (M17.26) -------------------------------------------------------
//
// A smooth curve THROUGH a list of points, in order. Interpolating, not
// control-point: the points are on the curve, which is the kind a sketcher
// draws by clicking where the shape should go. The control-point form is the
// same storage read differently and is deliberately not here yet -- the two
// mean different things about the same numbers, and one flag deciding which
// would be the silent-reinterpretation defect this project keeps paying for.
//
// THE POINTS ARE THE STATE. Unlike an arc, whose tips are derived from a
// centre, radius and angle, a spline's points are its own variables -- so the
// solver holds them directly and there is nothing to derive.
//
// A spline through n points carries 2n degrees of freedom, and the DOF readout
// says so. Only its FIRST and LAST are addressable by a constraint, because
// SketchElementRef names a sub-element and there are only four of them; an
// interior point cannot be named until a reference can carry an index. That is
// a real limit, and it is stated rather than worked around -- joining a
// spline's ends to its neighbours is what a profile needs, and it is what
// works.
struct SketchSpline {
    // At least two, in the order the curve visits them.
    std::vector<Vec2> points;
    // A closed spline runs back to its first point smoothly, and forms an
    // entire loop on its own the way a circle does.
    bool closed{false};
};

using SketchGeometry =
    std::variant<SketchPoint, SketchLine, SketchCircle, SketchArc, SketchEllipse,
                 SketchEllipticalArc, SketchSpline>;

// Fewest points a spline can be drawn through. Two is a curve; one is a point
// wearing the wrong type.
inline constexpr std::size_t kMinSplinePoints = 2;

// The spline's own sampling, and THE one place its shape is decided in Core.
//
// A CATMULL-ROM cubic through the points: local, so moving one point changes
// only the two spans either side of it, which is what a user dragging a point
// expects. It passes through every given point exactly.
//
// The KERNEL builds the real B-spline from the same points (OCCT's own
// interpolation), so the two agree exactly AT the points and to within a
// fraction of a chord between them. That difference is real and bounded: this
// sampling decides drawing, picking and which loop contains which, and the
// kernel decides the solid. Where they must agree exactly -- the endpoints a
// profile chains through -- they do.
//
// `samplesPerSpan` is how finely each span between two given points is cut.
std::vector<Vec2> SampleSpline(const SketchSpline& spline, int samplesPerSpan);

// A point partway along, with `t` in [0,1] over the whole curve.
Vec2 PointOnSpline(const SketchSpline& spline, double t);

// The point at parameter `t`. THE one place the parametrisation lives, so the
// canvas, the solver, the kernel and the tests cannot disagree about where a
// parameter puts a point.
Vec2 PointOnEllipse(Vec2 centre, double majorRadiusMm, double minorRadiusMm, double rotationRad,
                    double paramRad) noexcept;

// The parameter of the point on the ellipse nearest the ray from the centre
// through `p`. The inverse of the above, and the only sanctioned way to get a
// parameter from a position.
double EllipseParamOf(Vec2 centre, double majorRadiusMm, double minorRadiusMm, double rotationRad,
                      Vec2 p) noexcept;

struct SketchEntity {
    SketchEntityId id{kInvalidSketchEntityId};
    SketchGeometry geometry{};

    // Construction geometry: drawn, snapped to, constrained and dimensioned
    // like anything else, but contributing NO edge to a profile.
    //
    // A FLAG, not a type (roadmap 4.1.1 point 2). Construction geometry is
    // still a Line, a Circle or an Arc -- it differs only in whether a pad may
    // sweep it. A parallel hierarchy of construction types would double every
    // switch in the solver, the serializer and the canvas to express one
    // boolean, and every one of those switches would be a place for the two
    // halves to drift apart.
    //
    // It is what makes Offset, Trim and Mirror usable in practice: the line you
    // measure from is usually not a line you want in the solid.
    bool construction{false};
};

// --- Projected reference geometry (M17.6, ADR-M17-029) ----------------------
//
// The picked face's boundary, projected into this sketch's (u,v) and held as a
// TRACING UNDERLAY. A reference is not an entity:
//
//   * it has no variables, so the solver never sees it and it cannot move;
//   * it contributes no edge to a profile;
//   * no constraint may name it.
//
// It is kept in its OWN container for exactly those reasons. Modelling it as
// an entity with a third flag would put it inside every loop that walks
// entities_ -- the solver, the profile builder, the serializer, the canvas,
// undo -- and each of those would then need to remember to skip it. One
// forgotten skip is a reference edge silently becoming part of the solid.
// A separate vector cannot be forgotten, because it is never reached by
// accident.
//
// A reference is FROZEN, the same way the sketch's plane is frozen
// (ADR-M17-028): it is what the face looked like when the sketch was made. It
// does not track the face. To get geometry that participates in the model, a
// user CONVERTS it -- which creates an ordinary entity, and from that moment
// the two have nothing to do with each other.
enum class SketchReferenceId : ObjectId {};

constexpr SketchReferenceId kInvalidSketchReferenceId{kInvalidObjectId};

inline ObjectId ToObjectId(SketchReferenceId id) noexcept {
    return static_cast<ObjectId>(id);
}

inline SketchReferenceId NextSketchReferenceId() {
    return static_cast<SketchReferenceId>(ObjectIdGenerator::Next());
}

inline SketchReferenceId RestoreSketchReferenceId(SketchReferenceId id) {
    ObjectIdGenerator::AdvancePast(ToObjectId(id));
    return id;
}

struct SketchReference {
    SketchReferenceId id{kInvalidSketchReferenceId};
    // The SAME geometry variant an entity uses, because a projected edge IS a
    // line, a circle, an arc or a vertex -- and reusing the type means Convert
    // is a copy rather than a translation that could disagree with itself.
    SketchGeometry geometry{};
};

// THE point a reference names, as a position -- or nullopt when the entity has
// no such part (M17.30).
//
// One place, because "where is Spline1.p3" is asked by the canvas (to draw a
// handle), by the picker (to snap to it), by the reconstructor (to check a
// plan) and by the constraint list (to describe one). Four copies of an index
// bounds-check is four chances to read one past the end.
std::optional<Vec2> PointOfSubElement(const SketchGeometry& geometry, SketchSubElement part,
                                      int index) noexcept;

// Whether a reference is one this model can resolve at all: the sub-element has
// to exist on that kind of entity, and a SplinePoint's index has to be one of
// the INTERIOR points -- index 0 and index n-1 already have names, and a second
// spelling of the same point would compare unequal to itself.
bool IsResolvableRef(const SketchGeometry& geometry, SketchSubElement part, int index) noexcept;

// Smallest length this sketch model treats as non-degenerate, in mm. Shared
// with the profile connectivity tolerance (ADR-M4-005) so the project keeps one
// length-scale story: this is the same 1e-6 mm used by M3's kLengthAbsTol and
// kMinBoxDimensionMm.
inline constexpr double kSketchToleranceMm = 1e-6;

// The ONE place sketch geometry validation lives, mirroring
// IsValidBoxDefinition's role at the kernel boundary (ADR-M3-001).
//
// Applied in two places by design (defense in depth, the same pattern
// MassPropertiesNode uses for its upstream check): Sketch's add* methods reject
// invalid geometry so it never enters the model, AND the profile validator
// re-checks, because a restored entity can carry bad data from a hand-edited
// document that never went through add*.
bool IsValidSketchGeometry(const SketchGeometry& geometry) noexcept;

// True for geometry that has distinct start/end points a profile loop can be
// chained through -- false for Point (not a curve) and Circle (closed, so it
// forms a one-entity loop by itself, ADR-M4-005).
bool HasEndpoints(const SketchGeometry& geometry) noexcept;

// Endpoints in sketch-local (u,v). Valid only when HasEndpoints() is true;
// returns the origin otherwise, never undefined behaviour.
Vec2 StartPointOf(const SketchGeometry& geometry) noexcept;
Vec2 EndPointOf(const SketchGeometry& geometry) noexcept;

// A point partway along the curve, used to tell apart two curves that share
// both endpoints. Endpoints alone are not enough: an arc and its chord
// legitimately span the same two points (a half-disc), and so do the two arcs
// of a lens -- while two arcs on OPPOSITE sides of the same chord also share
// endpoints but are different curves. The midpoint separates all these cases.
Vec2 MidPointOf(const SketchGeometry& geometry) noexcept;

// Whether two geometries hold the SAME VALUE, field for field.
//
// EXACT comparison, no tolerance: the callers are asking "did this change",
// and a tolerance would drop a genuine sub-micron move from an undo record or
// from the list of what a solve wrote.
//
// ONE function, because there were two. PartDocument had it for deciding what a
// drag moved and SketchSolveSession had it for deciding what a solve wrote, and
// both were get_if chains ending in a bare `std::get<SketchArc>` -- so when the
// variant grew a sixth alternative, the second one threw `bad_variant_access`
// from a noexcept function on the commit path of every solve that touched a
// spline, and the process simply died. Exhaustive, in one place, so the next
// alternative breaks the build instead.
bool SameSketchGeometryValue(const SketchGeometry& a, const SketchGeometry& b) noexcept;

// True when two entities describe the same curve, allowing for opposite
// direction. This is what "duplicate" means (spec 10) -- sharing endpoints is
// NOT sufficient, or every valid two-entity loop would be rejected.
bool IsSameCurve(const SketchGeometry& a, const SketchGeometry& b, double toleranceMm) noexcept;

// Squared distance in (u,v) mm. Squared so callers can compare against a
// squared tolerance without a sqrt in the connectivity inner loop.
inline double DistanceSquared(Vec2 a, Vec2 b) noexcept {
    const double du = a.x - b.x;
    const double dv = a.y - b.y;
    return du * du + dv * dv;
}

inline bool SamePoint(Vec2 a, Vec2 b, double toleranceMm) noexcept {
    return DistanceSquared(a, b) <= toleranceMm * toleranceMm;
}

} // namespace paramcad
