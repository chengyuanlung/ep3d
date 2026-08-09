#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Geometry/MathTypes.h"
#include <cmath>
#include <variant>

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
enum class SketchSubElement { Whole, StartPoint, EndPoint, CenterPoint };

struct SketchElementRef {
    SketchEntityId entityId{kInvalidSketchEntityId};
    SketchSubElement subElement{SketchSubElement::Whole};
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

using SketchGeometry = std::variant<SketchPoint, SketchLine, SketchCircle, SketchArc>;

struct SketchEntity {
    SketchEntityId id{kInvalidSketchEntityId};
    SketchGeometry geometry{};
};

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
