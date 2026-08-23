#include "Core/Sketch/SketchSolveSession.h"

#include "Core/Document/ObjectRegistry.h"
#include "Core/Parameter/Parameter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <optional>
#include <variant>
#include <vector>

namespace paramcad {

namespace {

std::string IdText(SketchConstraintId id) { return std::to_string(ToObjectId(id)); }

// Where an entity's scalars live in the variable vector. Solver variable
// indices are an internal numbering rebuilt on every solve and are NEVER
// persisted (ADR-M5-001) -- this map exists only for the duration of one build.
struct EntitySlots {
    int startU{-1}, startV{-1}; // line start, or point, or circle/arc centre
    int endU{-1}, endV{-1};     // line end only
    int radius{-1};             // circle/arc only
    // M17, arcs only: the two sweep angles, and the two TIPS they place. The
    // tips are variables rather than a formula so that every constraint which
    // already holds a point holds an arc's end too (see ArcTipU).
    int startAngle{-1}, endAngle{-1};
    int tipStartU{-1}, tipStartV{-1};
    int tipEndU{-1}, tipEndV{-1};
    // M17.25, ellipses only. `radius` above holds the MAJOR one, and
    // startAngle/endAngle hold the two PARAMETERS -- the slots are reused so
    // that SlotFor and every point-holding constraint work unchanged, while the
    // variables themselves carry Component::MinorRadius, Rotation, StartParam
    // and EndParam so the packing guard still knows what it is looking at.
    int minorRadius{-1}, rotation{-1};
    // M17.26, splines only: ONE PAIR PER POINT, because a spline's shape is its
    // points and there is no fixed number of them.
    //
    // The first entity whose variable count is not a property of its TYPE. Every
    // other kind here fits in the named fields above; a spline through seven
    // points has fourteen variables and a spline through three has six, so the
    // struct grew a vector rather than the eighth, ninth and tenth field.
    std::vector<std::pair<int, int>> splinePoints;
};

const Parameter* ResolveParameter(const ObjectRegistry& registry, ObjectId id) {
    if (id == kInvalidObjectId) return nullptr;
    const std::optional<ObjectRegistry::ConstObjectRef> ref = registry.find(id);
    if (!ref) return nullptr;
    auto* const* parameter = std::get_if<const Parameter*>(&*ref);
    return parameter != nullptr ? *parameter : nullptr;
}

// A dimensional constraint's Parameter must carry a compatible unit. Spec 32
// lists "unit mismatch changes physical geometry" as a Critical example, and
// refusing the binding is how that defect stops being reachable rather than
// merely defended against (ADR-M5-002).
bool UnitMatches(const SketchConstraintData& data, UnitType unit) noexcept {
    // Unitless used to satisfy BOTH branches, so ONE unitless Parameter could
    // drive a Length and an Angle interchangeably -- the door this rule exists
    // to close, left ajar. A dimension states a physical quantity, and "no
    // unit" is not one of them.
    // THE ANGLE-VALUED KINDS, listed. An ellipse's orientation is one of them,
    // and the day it was added this function still said "Angle or millimetres"
    // -- so the constraint was built, refused for a unit mismatch, and the
    // ellipse quietly kept spinning free. Nothing named it: the sketch simply
    // reported one more degree of freedom than the user thought they had spent.
    if (std::holds_alternative<AngleConstraint>(data) ||
        std::holds_alternative<EllipseRotationConstraint>(data))
        return unit == UnitType::Radian;
    return unit == UnitType::Millimeter;
}

bool DimensionValueValid(const SketchConstraintData& data, double value) noexcept {
    if (!std::isfinite(value)) return false;
    // Angles may be any finite value. So may the SIGNED separations, and for
    // the same reason: their sign and their zero both mean something.
    //
    // ADR-M5-002's rule -- a length, a radius or a point-to-point distance must
    // be strictly positive -- is about quantities that have no meaning at or
    // below zero: a zero-length line has no direction, a zero-radius circle is
    // a point. A horizontal separation of zero says the two points are
    // vertically aligned, and a negative one says which side; a point ON a line
    // is at distance zero from it. Refusing those made the behaviour
    // HorizontalDistanceConstraint documents unreachable -- the constraint was
    // rejected as InvalidInput the moment the user typed the negative value it
    // says they may type.
    //
    // AN ELLIPSE'S ORIENTATION belongs here too, and zero is the case that
    // proves it: an ellipse whose major axis lies along +u has a rotation of
    // exactly 0, which is a perfectly ordinary thing to pin. Left out, the
    // constraint was rejected the moment it was created -- and the rejection
    // named a Parameter value rather than saying the ellipse was still free.
    if (std::holds_alternative<AngleConstraint>(data) ||
        std::holds_alternative<EllipseRotationConstraint>(data) ||
        std::holds_alternative<HorizontalDistanceConstraint>(data) ||
        std::holds_alternative<VerticalDistanceConstraint>(data) ||
        std::holds_alternative<PointLineDistanceConstraint>(data))
        return true;
    return value >= kMinSketchDimensionMm;
}

// Which scalar of an entity a SketchElementRef names, or -1 if the reference is
// meaningless for that entity's kind.
//
// Keyed on the ENTITY KIND, exhaustively, in both directions. An earlier version
// asked only `isLine` and guarded EndPoint, CenterPoint and Whole with it --
// leaving StartPoint unguarded, so `Fix(circle.StartPoint)` silently resolved to
// the circle's CENTRE and solved to a plausible-looking wrong answer. The test
// that existed checked CenterPoint-on-a-line, the one direction that happened to
// be guarded; the symmetric case was never asked. A wrong sub-element must
// FAIL, never quietly become a different element.
int SlotFor(const EntitySlots& slots, const SketchEntity& entity, SketchSubElement part,
            int index, bool wantU) {
    if (std::holds_alternative<SketchLine>(entity.geometry)) {
        // A line has two endpoints and nothing else. "Whole" is ambiguous for
        // it, and it has no centre.
        if (part == SketchSubElement::StartPoint) return wantU ? slots.startU : slots.startV;
        if (part == SketchSubElement::EndPoint) return wantU ? slots.endU : slots.endV;
        return -1;
    }
    if (std::holds_alternative<SketchArc>(entity.geometry)) {
        // An arc offers its centre AND its two tips (M17). The tips are
        // variables tied to the centre, radius and angle by ArcTipU/V, so a
        // constraint naming one holds a real point rather than resolving to
        // the radius -- which is what ADR-M12-003 was written to prevent and
        // what this replaces.
        if (part == SketchSubElement::CenterPoint) return wantU ? slots.startU : slots.startV;
        if (part == SketchSubElement::StartPoint) return wantU ? slots.tipStartU : slots.tipStartV;
        if (part == SketchSubElement::EndPoint) return wantU ? slots.tipEndU : slots.tipEndV;
        return -1;
    }
    if (const auto* spline = std::get_if<SketchSpline>(&entity.geometry)) {
        if (slots.splinePoints.empty()) return -1;
        // ANY POINT, by index (M17.30). The ends keep their own names and an
        // interior point is a SplinePoint -- one spelling each, checked by
        // IsResolvableRef before it gets here.
        //
        // A CLOSED spline has no ends, the way a circle has none, so its points
        // are ALL reached by index.
        int which = -1;
        if (part == SketchSubElement::SplinePoint) which = index;
        else if (!spline->closed && part == SketchSubElement::StartPoint) which = 0;
        else if (!spline->closed && part == SketchSubElement::EndPoint)
            which = static_cast<int>(slots.splinePoints.size()) - 1;
        if (which < 0 || which >= static_cast<int>(slots.splinePoints.size())) return -1;
        const auto& pair = slots.splinePoints[static_cast<std::size_t>(which)];
        return wantU ? pair.first : pair.second;
    }
    if (std::holds_alternative<SketchEllipticalArc>(entity.geometry)) {
        // The same three as an arc, and for the same reason: the tips are
        // variables (EllipseTipU/V), so a coincidence on one holds a real point.
        if (part == SketchSubElement::CenterPoint) return wantU ? slots.startU : slots.startV;
        if (part == SketchSubElement::StartPoint) return wantU ? slots.tipStartU : slots.tipStartV;
        if (part == SketchSubElement::EndPoint) return wantU ? slots.tipEndU : slots.tipEndV;
        return -1;
    }
    if (std::holds_alternative<SketchCircle>(entity.geometry) ||
        std::holds_alternative<SketchEllipse>(entity.geometry)) {
        // CLOSED -- no ends, and the only referenceable point is the centre.
        if (part == SketchSubElement::CenterPoint) return wantU ? slots.startU : slots.startV;
        return -1;
    }
    // A point IS its position: "Whole" is the only reference that means
    // anything, and it is stored in the start slots.
    if (part == SketchSubElement::Whole) return wantU ? slots.startU : slots.startV;
    return -1;
}

} // namespace

BuildProblemResult BuildSolveProblem(const Sketch& sketch, const ObjectRegistry& registry) {
    BuildProblemResult out;

    // --- Variables, in ascending entity id order ---------------------------
    std::vector<const SketchEntity*> entities;
    entities.reserve(sketch.entities().size());
    for (const SketchEntity& entity : sketch.entities()) entities.push_back(&entity);
    std::sort(entities.begin(), entities.end(),
              [](const SketchEntity* a, const SketchEntity* b) {
                  return ToObjectId(a->id) < ToObjectId(b->id);
              });

    std::map<ObjectId, EntitySlots> slots;
    const auto addVariable = [&](SketchEntityId id, SketchSubElement part,
                                 SolveVariable::Component component, double value) {
        out.problem.variables.push_back(SolveVariable{id, part, component});
        out.problem.initialValues.push_back(value);
        return static_cast<int>(out.problem.variables.size()) - 1;
    };

    for (const SketchEntity* entity : entities) {
        EntitySlots s;
        std::visit(
            [&](const auto& geometry) {
                using T = std::decay_t<decltype(geometry)>;
                if constexpr (std::is_same_v<T, SketchPoint>) {
                    s.startU = addVariable(entity->id, SketchSubElement::Whole,
                                           SolveVariable::Component::U, geometry.position.x);
                    s.startV = addVariable(entity->id, SketchSubElement::Whole,
                                           SolveVariable::Component::V, geometry.position.y);
                } else if constexpr (std::is_same_v<T, SketchLine>) {
                    s.startU = addVariable(entity->id, SketchSubElement::StartPoint,
                                           SolveVariable::Component::U, geometry.start.x);
                    s.startV = addVariable(entity->id, SketchSubElement::StartPoint,
                                           SolveVariable::Component::V, geometry.start.y);
                    s.endU = addVariable(entity->id, SketchSubElement::EndPoint,
                                         SolveVariable::Component::U, geometry.end.x);
                    s.endV = addVariable(entity->id, SketchSubElement::EndPoint,
                                         SolveVariable::Component::V, geometry.end.y);
                } else if constexpr (std::is_same_v<T, SketchCircle>) {
                    s.startU = addVariable(entity->id, SketchSubElement::CenterPoint,
                                           SolveVariable::Component::U, geometry.center.x);
                    s.startV = addVariable(entity->id, SketchSubElement::CenterPoint,
                                           SolveVariable::Component::V, geometry.center.y);
                    s.radius = addVariable(entity->id, SketchSubElement::Whole,
                                           SolveVariable::Component::Radius, geometry.radiusMm);
                } else if constexpr (std::is_same_v<T, SketchSpline>) {
                    // TWO PER POINT. The sub-element recorded is StartPoint for
                    // the first and EndPoint for the last, so those two resolve
                    // the way every other curve's ends do; the interior ones are
                    // recorded as Whole, which no reference resolves to for a
                    // spline -- they are variables the solver moves and nothing
                    // names.
                    const std::size_t count = geometry.points.size();
                    for (std::size_t i = 0; i < count; ++i) {
                        const SketchSubElement part =
                            i == 0 ? SketchSubElement::StartPoint
                                   : (i + 1 == count ? SketchSubElement::EndPoint
                                                     : SketchSubElement::Whole);
                        const int u = addVariable(entity->id, part, SolveVariable::Component::U,
                                                  geometry.points[i].x);
                        const int v = addVariable(entity->id, part, SolveVariable::Component::V,
                                                  geometry.points[i].y);
                        s.splinePoints.emplace_back(u, v);
                    }
                } else if constexpr (std::is_same_v<T, SketchEllipse> ||
                                     std::is_same_v<T, SketchEllipticalArc>) {
                    // FIVE for a closed ellipse -- centre, both radii and the
                    // rotation -- and five more for an open one, which is two
                    // parameters and the two tips they place.
                    s.startU = addVariable(entity->id, SketchSubElement::CenterPoint,
                                           SolveVariable::Component::U, geometry.center.x);
                    s.startV = addVariable(entity->id, SketchSubElement::CenterPoint,
                                           SolveVariable::Component::V, geometry.center.y);
                    s.radius = addVariable(entity->id, SketchSubElement::Whole,
                                           SolveVariable::Component::Radius,
                                           geometry.majorRadiusMm);
                    s.minorRadius = addVariable(entity->id, SketchSubElement::Whole,
                                                SolveVariable::Component::MinorRadius,
                                                geometry.minorRadiusMm);
                    s.rotation = addVariable(entity->id, SketchSubElement::Whole,
                                             SolveVariable::Component::Rotation,
                                             geometry.rotationRad);
                    if constexpr (std::is_same_v<T, SketchEllipticalArc>) {
                        s.startAngle = addVariable(entity->id, SketchSubElement::StartPoint,
                                                   SolveVariable::Component::StartParam,
                                                   geometry.startParamRad);
                        s.endAngle = addVariable(entity->id, SketchSubElement::EndPoint,
                                                 SolveVariable::Component::EndParam,
                                                 geometry.endParamRad);
                        const Vec2 tipStart = PointOnEllipse(
                            geometry.center, geometry.majorRadiusMm, geometry.minorRadiusMm,
                            geometry.rotationRad, geometry.startParamRad);
                        const Vec2 tipEnd = PointOnEllipse(
                            geometry.center, geometry.majorRadiusMm, geometry.minorRadiusMm,
                            geometry.rotationRad, geometry.endParamRad);
                        s.tipStartU = addVariable(entity->id, SketchSubElement::StartPoint,
                                                  SolveVariable::Component::U, tipStart.x);
                        s.tipStartV = addVariable(entity->id, SketchSubElement::StartPoint,
                                                  SolveVariable::Component::V, tipStart.y);
                        s.tipEndU = addVariable(entity->id, SketchSubElement::EndPoint,
                                                SolveVariable::Component::U, tipEnd.x);
                        s.tipEndV = addVariable(entity->id, SketchSubElement::EndPoint,
                                                SolveVariable::Component::V, tipEnd.y);
                    }
                } else {
                    static_assert(std::is_same_v<T, SketchArc>);
                    // M5 held an arc's angular extent FIXED, on the grounds
                    // that no constraint could drive an arc endpoint and free
                    // angles would be DOF nothing could remove. M17 makes both
                    // halves false: a fillet's whole purpose is that its arc's
                    // tips stay on two lines, and Coincident holds them there.
                    //
                    // Reporting 3 DOF for a free arc was also simply WRONG -- a
                    // drawn arc has five independent quantities, and the reader
                    // was under-counting its freedom by two.
                    s.startU = addVariable(entity->id, SketchSubElement::CenterPoint,
                                           SolveVariable::Component::U, geometry.center.x);
                    s.startV = addVariable(entity->id, SketchSubElement::CenterPoint,
                                           SolveVariable::Component::V, geometry.center.y);
                    s.radius = addVariable(entity->id, SketchSubElement::Whole,
                                           SolveVariable::Component::Radius, geometry.radiusMm);
                    s.startAngle =
                        addVariable(entity->id, SketchSubElement::StartPoint,
                                    SolveVariable::Component::StartAngle, geometry.startAngleRad);
                    s.endAngle =
                        addVariable(entity->id, SketchSubElement::EndPoint,
                                    SolveVariable::Component::EndAngle, geometry.endAngleRad);
                    // The tips themselves, seeded where the angles put them.
                    const Vec2 tipStart{
                        geometry.center.x + geometry.radiusMm * std::cos(geometry.startAngleRad),
                        geometry.center.y + geometry.radiusMm * std::sin(geometry.startAngleRad)};
                    const Vec2 tipEnd{
                        geometry.center.x + geometry.radiusMm * std::cos(geometry.endAngleRad),
                        geometry.center.y + geometry.radiusMm * std::sin(geometry.endAngleRad)};
                    s.tipStartU = addVariable(entity->id, SketchSubElement::StartPoint,
                                              SolveVariable::Component::U, tipStart.x);
                    s.tipStartV = addVariable(entity->id, SketchSubElement::StartPoint,
                                              SolveVariable::Component::V, tipStart.y);
                    s.tipEndU = addVariable(entity->id, SketchSubElement::EndPoint,
                                            SolveVariable::Component::U, tipEnd.x);
                    s.tipEndV = addVariable(entity->id, SketchSubElement::EndPoint,
                                            SolveVariable::Component::V, tipEnd.y);
                }
            },
            entity->geometry);
        slots[ToObjectId(entity->id)] = s;
    }

    // --- What an arc's tips MEAN ------------------------------------------
    //
    // Emitted before any constraint's residual, and not attached to a
    // constraint at all: these are not something the user asked for, they are
    // the definition of where an arc's ends are. Two per tip, so a tip costs
    // two variables and two equations and adds no freedom of its own.
    for (const SketchEntity& entity : sketch.entities()) {
        const auto* arc = std::get_if<SketchArc>(&entity.geometry);
        if (arc == nullptr) continue;
        const EntitySlots& s = slots[ToObjectId(entity.id)];
        const auto bind = [&](int tipU, int tipV, int angle) {
            SolveResidual residual;
            residual.kind = SolveResidual::Kind::ArcTipU;
            residual.vars[0] = tipU;
            residual.vars[1] = s.startU;
            residual.vars[2] = s.radius;
            residual.vars[3] = angle;
            out.problem.residuals.push_back(residual);
            residual.kind = SolveResidual::Kind::ArcTipV;
            residual.vars[0] = tipV;
            residual.vars[1] = s.startV;
            residual.vars[2] = s.radius;
            residual.vars[3] = angle;
            out.problem.residuals.push_back(residual);
        };
        bind(s.tipStartU, s.tipStartV, s.startAngle);
        bind(s.tipEndU, s.tipEndV, s.endAngle);
    }

    // ...and the same for an ELLIPTICAL arc's tips, which need a bigger formula
    // because a rotated ellipse mixes the two axes (see EllipseTipU).
    for (const SketchEntity& entity : sketch.entities()) {
        if (!std::holds_alternative<SketchEllipticalArc>(entity.geometry)) continue;
        const EntitySlots& s = slots[ToObjectId(entity.id)];
        const auto bind = [&](int tipU, int tipV, int param) {
            SolveResidual residual;
            residual.kind = SolveResidual::Kind::EllipseTipU;
            residual.vars[0] = tipU;
            residual.vars[1] = s.startU;
            residual.vars[2] = s.radius;
            residual.vars[3] = s.minorRadius;
            residual.vars[4] = s.rotation;
            residual.vars[5] = param;
            out.problem.residuals.push_back(residual);
            residual.kind = SolveResidual::Kind::EllipseTipV;
            residual.vars[0] = tipV;
            residual.vars[1] = s.startV;
            out.problem.residuals.push_back(residual);
        };
        bind(s.tipStartU, s.tipStartV, s.startAngle);
        bind(s.tipEndU, s.tipEndV, s.endAngle);
    }

    // --- Residuals, in ascending constraint id order -----------------------
    std::vector<const SketchConstraint*> constraints;
    constraints.reserve(sketch.constraints().size());
    for (const SketchConstraint& c : sketch.constraints()) constraints.push_back(&c);
    std::sort(constraints.begin(), constraints.end(),
              [](const SketchConstraint* a, const SketchConstraint* b) {
                  return ToObjectId(a->id) < ToObjectId(b->id);
              });

    const auto reject = [&](SketchConstraintId id, const std::string& why) {
        out.invalidConstraints.push_back(id);
        if (out.message.empty())
            out.message = "constraint " + IdText(id) + ": " + why;
    };

    // Resolves a SketchElementRef to its (u, v) variable indices, or reports
    // why it cannot be resolved.
    const auto resolveRef = [&](const SketchElementRef& ref, int& u, int& v) -> const char* {
        const SketchEntity* entity = sketch.findEntity(ref.entityId);
        if (entity == nullptr) return "references an entity that is not in this sketch";
        const auto it = slots.find(ToObjectId(ref.entityId));
        if (it == slots.end()) return "references an entity with no solver variables";
        // REFUSED BEFORE IT IS RESOLVED, so the message can say which rule was
        // broken. A SplinePoint naming index 0 of an open spline is the case
        // that matters: it resolves perfectly well and is a SECOND SPELLING of
        // StartPoint, so the same point would compare unequal to itself.
        if (!IsResolvableRef(entity->geometry, ref.subElement, ref.index))
            return ref.subElement == SketchSubElement::SplinePoint
                       ? "names a spline point that is not an interior one; the first and last "
                         "are its start and its end"
                       : "names a sub-element this entity does not have";
        u = SlotFor(it->second, *entity, ref.subElement, ref.index, true);
        v = SlotFor(it->second, *entity, ref.subElement, ref.index, false);
        if (u < 0 || v < 0) return "names a sub-element this entity does not have";
        return nullptr;
    };

    for (const SketchConstraint* constraint : constraints) {
        const SketchConstraintData& data = constraint->data;
        const SketchConstraintId id = constraint->id;

        // A DRIVEN dimension contributes NO residual (M17.19, ADR-M17-042).
        //
        // That is the whole of it in the solver: it measures rather than
        // drives, so it must not remove a degree of freedom and must not be
        // able to conflict with anything. Its Parameter is written afterwards,
        // from the solved geometry, by the caller that owns the commit --
        // writing it here would mean the solver mutating a document object
        // mid-solve, and doing so from a function that also decides whether
        // the solve is possible.
        //
        // Recorded, not silently skipped: a caller has to know which
        // constraints to measure afterwards, and finding them by re-testing
        // the flag in a second place is how the two lists come to disagree.
        if (constraint->driven && IsDimensional(data)) {
            out.drivenConstraints.push_back(id);
            continue;
        }

        // Dimensional constraints: resolve and validate the Parameter first.
        double target = 0.0;
        // Belt and braces with the facade: an unbound dimensional constraint is
        // InvalidInput, never a silent target of 0. Reaching the solver with
        // target 0 asked it to drive a line to zero length and a circle to zero
        // radius -- values ADR-M5-002 declares invalid.
        if (IsDimensional(data) && BoundParameterId(data) == kInvalidObjectId) {
            reject(id, "a dimensional constraint is not bound to any Parameter");
            continue;
        }
        const ObjectId parameterId = BoundParameterId(data);
        if (parameterId != kInvalidObjectId) {
            const Parameter* parameter = ResolveParameter(registry, parameterId);
            if (parameter == nullptr) {
                reject(id, "bound Parameter is not in this document");
                continue;
            }
            if (!UnitMatches(data, parameter->unit())) {
                reject(id, "bound Parameter carries an incompatible unit");
                continue;
            }
            target = parameter->value();
            if (!DimensionValueValid(data, target)) {
                reject(id, "bound Parameter value is not a valid dimension");
                continue;
            }
        }

        const auto lineSlots = [&](SketchEntityId lineId,
                                   const EntitySlots** found) -> const char* {
            const SketchEntity* entity = sketch.findEntity(lineId);
            if (entity == nullptr) return "references an entity that is not in this sketch";
            if (!std::holds_alternative<SketchLine>(entity->geometry))
                return "requires a line";
            *found = &slots.at(ToObjectId(lineId));
            return nullptr;
        };
        // A line long enough to have a direction. Only angles need this: a
        // Length on a zero-length line is solvable -- its direction is simply a
        // free degree of freedom -- whereas an Angle to it is not defined at
        // all. That first half was asserted here before it was true: the
        // distance residual's Jacobian row is all zeros at zero separation, so
        // such a Length was reported Conflicting until the solver learned to
        // nudge a degenerate starting configuration.
        const auto requireDirected = [&](SketchEntityId lineId) -> const char* {
            const SketchEntity* entity = sketch.findEntity(lineId);
            if (entity == nullptr) return "references an entity that is not in this sketch";
            const auto* line = std::get_if<SketchLine>(&entity->geometry);
            if (line == nullptr) return "requires a line";
            const double du = line->end.x - line->start.x;
            const double dv = line->end.y - line->start.y;
            if (!std::isfinite(du) || !std::isfinite(dv)) return "line has non-finite endpoints";
            if (std::hypot(du, dv) < kMinSketchDimensionMm)
                return "angle needs a line with a direction, and this line has zero length";
            return nullptr;
        };
        // A SPLINE'S END, as the two point-slots that decide which way the
        // curve leaves it: the end itself, and the point next to it.
        //
        // The chord IS the tangent, exactly -- not an approximation. Core
        // evaluates a spline as a uniform Catmull-Rom whose ends are REFLECTED
        // (`SplinePointAt`), and that reflection makes the derivative at the
        // first point come out as
        //
        //     0.5 * (p1 - (2*p0 - p1)) = p1 - p0
        //
        // and by the same algebra at the last point as p[n-1] - p[n-2]. So
        // "which way does this spline leave its end" is a question about two
        // points, both of which are already solver variables. That is what lets
        // spline tangency reuse the residuals that already exist instead of
        // needing a new one.
        struct SplineEnd {
            int endU{-1}, endV{-1};
            int nextU{-1}, nextV{-1};
        };
        const auto splineEnd = [&](SketchEntityId splineId, SketchSubElement at,
                                   SplineEnd* found) -> const char* {
            const SketchEntity* entity = sketch.findEntity(splineId);
            if (entity == nullptr) return "references an entity that is not in this sketch";
            const auto* spline = std::get_if<SketchSpline>(&entity->geometry);
            if (spline == nullptr) return "requires a spline";
            if (spline->closed)
                // A CLOSED spline has no ends, the way a circle has none. Naming
                // one would have to mean some point by index, and that is a
                // different constraint than the one asked for.
                return "a closed spline has no ends to be tangent at";
            const EntitySlots& mine = slots.at(ToObjectId(splineId));
            const int count = static_cast<int>(mine.splinePoints.size());
            if (count < 2) return "a spline needs two points before it has a direction";
            const bool atStart = at == SketchSubElement::StartPoint;
            const auto& tip = atStart ? mine.splinePoints.front() : mine.splinePoints.back();
            const auto& next = atStart ? mine.splinePoints[1]
                                       : mine.splinePoints[static_cast<std::size_t>(count - 2)];
            *found = SplineEnd{tip.first, tip.second, next.first, next.second};
            return nullptr;
        };
        const auto curveSlots = [&](SketchEntityId curveId,
                                    const EntitySlots** found) -> const char* {
            const SketchEntity* entity = sketch.findEntity(curveId);
            if (entity == nullptr) return "references an entity that is not in this sketch";
            if (std::holds_alternative<SketchEllipse>(entity->geometry) ||
                std::holds_alternative<SketchEllipticalArc>(entity->geometry))
                // NAMED, rather than folded into the message below. An ellipse
                // HAS a radius slot -- the major one -- so a constraint that
                // took it would solve and silently drive one axis of two. Being
                // told which command to use instead is the difference between
                // a refusal and a dead end.
                return "an ellipse has two radii: dimension its major or minor axis instead";
            if (!std::holds_alternative<SketchCircle>(entity->geometry) &&
                !std::holds_alternative<SketchArc>(entity->geometry))
                return "requires a circle or an arc";
            *found = &slots.at(ToObjectId(curveId));
            return nullptr;
        };
        // Anything with a CENTRE, which is all a Concentric needs. Kept apart
        // from curveSlots above because that one also promises a single radius.
        const auto centredSlots = [&](SketchEntityId curveId,
                                      const EntitySlots** found) -> const char* {
            const SketchEntity* entity = sketch.findEntity(curveId);
            if (entity == nullptr) return "references an entity that is not in this sketch";
            if (!std::holds_alternative<SketchCircle>(entity->geometry) &&
                !std::holds_alternative<SketchArc>(entity->geometry) &&
                !std::holds_alternative<SketchEllipse>(entity->geometry) &&
                !std::holds_alternative<SketchEllipticalArc>(entity->geometry))
                return "requires a circle, an arc or an ellipse";
            *found = &slots.at(ToObjectId(curveId));
            return nullptr;
        };

        SolveResidual residual;
        residual.sourceConstraint = id;

        if (const auto* c = std::get_if<CoincidentConstraint>(&data)) {
            int au = -1, av = -1, bu = -1, bv = -1;
            if (const char* why = resolveRef(c->a, au, av)) { reject(id, why); continue; }
            if (const char* why = resolveRef(c->b, bu, bv)) { reject(id, why); continue; }
            residual.kind = SolveResidual::Kind::PointsEqualU;
            residual.vars[0] = au;
            residual.vars[1] = bu;
            out.problem.residuals.push_back(residual);
            residual.kind = SolveResidual::Kind::PointsEqualV;
            residual.vars[0] = av;
            residual.vars[1] = bv;
            out.problem.residuals.push_back(residual);

        } else if (const auto* c = std::get_if<HorizontalConstraint>(&data)) {
            const EntitySlots* s = nullptr;
            if (const char* why = lineSlots(c->line, &s)) { reject(id, why); continue; }
            residual.kind = SolveResidual::Kind::LineHorizontal;
            residual.vars[0] = s->startV;
            residual.vars[1] = s->endV;
            out.problem.residuals.push_back(residual);

        } else if (const auto* c = std::get_if<VerticalConstraint>(&data)) {
            const EntitySlots* s = nullptr;
            if (const char* why = lineSlots(c->line, &s)) { reject(id, why); continue; }
            residual.kind = SolveResidual::Kind::LineVertical;
            residual.vars[0] = s->startU;
            residual.vars[1] = s->endU;
            out.problem.residuals.push_back(residual);

        } else if (const auto* c = std::get_if<FixConstraint>(&data)) {
            int u = -1, v = -1;
            if (const char* why = resolveRef(c->target, u, v)) { reject(id, why); continue; }
            residual.kind = SolveResidual::Kind::FixedU;
            residual.vars[0] = u;
            residual.target = out.problem.initialValues[static_cast<std::size_t>(u)];
            out.problem.residuals.push_back(residual);
            residual.kind = SolveResidual::Kind::FixedV;
            residual.vars[0] = v;
            residual.target = out.problem.initialValues[static_cast<std::size_t>(v)];
            out.problem.residuals.push_back(residual);

        } else if (const auto* c = std::get_if<DistanceConstraint>(&data)) {
            int au = -1, av = -1, bu = -1, bv = -1;
            if (const char* why = resolveRef(c->a, au, av)) { reject(id, why); continue; }
            if (const char* why = resolveRef(c->b, bu, bv)) { reject(id, why); continue; }
            residual.kind = SolveResidual::Kind::Distance;
            residual.vars[0] = au;
            residual.vars[1] = av;
            residual.vars[2] = bu;
            residual.vars[3] = bv;
            residual.target = target;
            out.problem.residuals.push_back(residual);

        } else if (const auto* c = std::get_if<HorizontalDistanceConstraint>(&data)) {
            int au = -1, av = -1, bu = -1, bv = -1;
            if (const char* why = resolveRef(c->a, au, av)) { reject(id, why); continue; }
            if (const char* why = resolveRef(c->b, bu, bv)) { reject(id, why); continue; }
            // The U components only -- the V slots are resolved just to prove
            // both refs name real POINTS. A ref that resolves to half a point
            // is a modelling error, and finding it here beats discovering it as
            // a Jacobian column of zeros.
            residual.kind = SolveResidual::Kind::PointsDeltaU;
            residual.vars[0] = au;
            residual.vars[1] = bu;
            residual.target = target;
            out.problem.residuals.push_back(residual);

        } else if (const auto* c = std::get_if<VerticalDistanceConstraint>(&data)) {
            int au = -1, av = -1, bu = -1, bv = -1;
            if (const char* why = resolveRef(c->a, au, av)) { reject(id, why); continue; }
            if (const char* why = resolveRef(c->b, bu, bv)) { reject(id, why); continue; }
            residual.kind = SolveResidual::Kind::PointsDeltaV;
            residual.vars[0] = av;
            residual.vars[1] = bv;
            residual.target = target;
            out.problem.residuals.push_back(residual);

        } else if (const auto* c = std::get_if<LengthConstraint>(&data)) {
            const EntitySlots* s = nullptr;
            if (const char* why = lineSlots(c->line, &s)) { reject(id, why); continue; }
            residual.kind = SolveResidual::Kind::Length;
            residual.vars[0] = s->startU;
            residual.vars[1] = s->startV;
            residual.vars[2] = s->endU;
            residual.vars[3] = s->endV;
            residual.target = target;
            out.problem.residuals.push_back(residual);

        } else if (const auto* c = std::get_if<RadiusConstraint>(&data)) {
            const EntitySlots* s = nullptr;
            if (const char* why = curveSlots(c->curve, &s)) { reject(id, why); continue; }
            residual.kind = SolveResidual::Kind::Radius;
            residual.vars[0] = s->radius;
            residual.target = target;
            out.problem.residuals.push_back(residual);

        } else if (const auto* c = std::get_if<DiameterConstraint>(&data)) {
            const EntitySlots* s = nullptr;
            if (const char* why = curveSlots(c->curve, &s)) { reject(id, why); continue; }
            // Drives the SAME radius variable, halved -- not separate state
            // (ADR-M5-002). A Radius and a Diameter that disagree therefore
            // conflict, which is the intended behaviour.
            // The value floor is checked against the DIAMETER above, but what
            // gets committed is the radius, so the floor has to follow the
            // halving: Diameter = 1e-6 passed validation and then wrote
            // radius = 5e-7, geometry the sketch's own validator rejects.
            if (target / 2.0 < kMinSketchDimensionMm) {
                reject(id, "diameter is too small: it implies a degenerate radius");
                continue;
            }
            residual.kind = SolveResidual::Kind::Radius;
            residual.vars[0] = s->radius;
            residual.target = target / 2.0;
            out.problem.residuals.push_back(residual);

        } else if (const auto* angle = std::get_if<AngleConstraint>(&data)) {
            const EntitySlots* a = nullptr;
            const EntitySlots* b = nullptr;
            if (const char* why = lineSlots(angle->lineA, &a)) { reject(id, why); continue; }
            if (const char* why = lineSlots(angle->lineB, &b)) { reject(id, why); continue; }
            // ADR-M5-006: an angle needs a DIRECTION, and a line shorter than
            // kMinSketchDimensionMm has none. Without this the residual is
            // atan2(0, 0) and the solver pulls the degenerate line's endpoints
            // apart to satisfy an angle that was never defined -- it silently
            // edits geometry the user did not ask it to touch. addLine already
            // rejects degenerate geometry, but restoreEntity deliberately does
            // not (a hand-edited file must round-trip), so the check has to
            // live here, where the constraint is actually used.
            if (const char* why = requireDirected(angle->lineA)) { reject(id, why); continue; }
            if (const char* why = requireDirected(angle->lineB)) { reject(id, why); continue; }
            residual.kind = SolveResidual::Kind::Angle;
            // BOTH components of BOTH endpoints of BOTH lines. An angle is a
            // function of two direction VECTORS; packing only the v components
            // (as an earlier revision did, because the residual type had four
            // slots and this needs eight) compares millimetres to radians.
            residual.vars[0] = a->startU;
            residual.vars[1] = a->startV;
            residual.vars[2] = a->endU;
            residual.vars[3] = a->endV;
            residual.vars[4] = b->startU;
            residual.vars[5] = b->startV;
            residual.vars[6] = b->endU;
            residual.vars[7] = b->endV;
            residual.target = target;
            out.problem.residuals.push_back(residual);

        // --- M13: the geometric constraints --------------------------------

        } else if (std::holds_alternative<ParallelConstraint>(data) ||
                   std::holds_alternative<PerpendicularConstraint>(data)) {
            const bool parallel = std::holds_alternative<ParallelConstraint>(data);
            const SketchEntityId lineAId =
                parallel ? std::get<ParallelConstraint>(data).lineA
                         : std::get<PerpendicularConstraint>(data).lineA;
            const SketchEntityId lineBId =
                parallel ? std::get<ParallelConstraint>(data).lineB
                         : std::get<PerpendicularConstraint>(data).lineB;
            const EntitySlots* a = nullptr;
            const EntitySlots* b = nullptr;
            if (const char* why = lineSlots(lineAId, &a)) { reject(id, why); continue; }
            if (const char* why = lineSlots(lineBId, &b)) { reject(id, why); continue; }
            // Same reasoning as Angle: both residuals are functions of two
            // DIRECTIONS, and a zero-length line has none. Their normalisation
            // divides by the lengths, so a degenerate line here is not merely
            // undefined -- it is 0/0.
            if (const char* why = requireDirected(lineAId)) { reject(id, why); continue; }
            if (const char* why = requireDirected(lineBId)) { reject(id, why); continue; }
            if (lineAId == lineBId) {
                reject(id, "a line cannot be parallel or perpendicular to itself");
                continue;
            }
            residual.kind = parallel ? SolveResidual::Kind::LinesParallel
                                     : SolveResidual::Kind::LinesPerpendicular;
            residual.vars[0] = a->startU;
            residual.vars[1] = a->startV;
            residual.vars[2] = a->endU;
            residual.vars[3] = a->endV;
            residual.vars[4] = b->startU;
            residual.vars[5] = b->startV;
            residual.vars[6] = b->endU;
            residual.vars[7] = b->endV;
            out.problem.residuals.push_back(residual);

        } else if (const auto* c = std::get_if<EqualConstraint>(&data)) {
            const SketchEntity* entityA = sketch.findEntity(c->a);
            const SketchEntity* entityB = sketch.findEntity(c->b);
            if (entityA == nullptr || entityB == nullptr) {
                reject(id, "references an entity that is not in this sketch");
                continue;
            }
            if (c->a == c->b) {
                reject(id, "an entity is always equal to itself");
                continue;
            }
            const bool linesA = std::holds_alternative<SketchLine>(entityA->geometry);
            const bool linesB = std::holds_alternative<SketchLine>(entityB->geometry);
            if (linesA && linesB) {
                const EntitySlots* a = nullptr;
                const EntitySlots* b = nullptr;
                if (const char* why = lineSlots(c->a, &a)) { reject(id, why); continue; }
                if (const char* why = lineSlots(c->b, &b)) { reject(id, why); continue; }
                residual.kind = SolveResidual::Kind::LengthsEqual;
                residual.vars[0] = a->startU;
                residual.vars[1] = a->startV;
                residual.vars[2] = a->endU;
                residual.vars[3] = a->endV;
                residual.vars[4] = b->startU;
                residual.vars[5] = b->startV;
                residual.vars[6] = b->endU;
                residual.vars[7] = b->endV;
                out.problem.residuals.push_back(residual);
            } else if (!linesA && !linesB) {
                const EntitySlots* a = nullptr;
                const EntitySlots* b = nullptr;
                if (const char* why = curveSlots(c->a, &a)) { reject(id, why); continue; }
                if (const char* why = curveSlots(c->b, &b)) { reject(id, why); continue; }
                residual.kind = SolveResidual::Kind::RadiiEqual;
                residual.vars[0] = a->radius;
                residual.vars[1] = b->radius;
                out.problem.residuals.push_back(residual);
            } else {
                // A length and a radius are not the same quantity, and
                // silently equating them would be a constraint whose meaning
                // depends on which entity the user happened to pick first.
                reject(id, "Equal needs two lines or two curves, not one of each");
                continue;
            }

        } else if (const auto* c = std::get_if<ConcentricConstraint>(&data)) {
            const EntitySlots* a = nullptr;
            const EntitySlots* b = nullptr;
            // CENTRES ONLY, so an ellipse qualifies: sharing a centre says
            // nothing about how many radii either side has.
            if (const char* why = centredSlots(c->curveA, &a)) { reject(id, why); continue; }
            if (const char* why = centredSlots(c->curveB, &b)) { reject(id, why); continue; }
            if (c->curveA == c->curveB) {
                reject(id, "a curve is always concentric with itself");
                continue;
            }
            // Reuses the coincidence residuals on the two CENTRES. Concentric
            // is a distinct constraint TYPE because that is what the user
            // means and what the list must say; it is not a distinct equation.
            residual.kind = SolveResidual::Kind::PointsEqualU;
            residual.vars[0] = a->startU;
            residual.vars[1] = b->startU;
            out.problem.residuals.push_back(residual);
            residual.kind = SolveResidual::Kind::PointsEqualV;
            residual.vars[0] = a->startV;
            residual.vars[1] = b->startV;
            out.problem.residuals.push_back(residual);

        } else if (const auto* c = std::get_if<MidpointConstraint>(&data)) {
            int pu = -1, pv = -1;
            if (const char* why = resolveRef(c->point, pu, pv)) { reject(id, why); continue; }
            const EntitySlots* line = nullptr;
            if (const char* why = lineSlots(c->line, &line)) { reject(id, why); continue; }
            if (c->point.entityId == c->line) {
                reject(id, "a line's own endpoint cannot be its midpoint");
                continue;
            }
            residual.kind = SolveResidual::Kind::MidpointU;
            residual.vars[0] = pu;
            residual.vars[1] = line->startU;
            residual.vars[2] = line->endU;
            out.problem.residuals.push_back(residual);
            residual.kind = SolveResidual::Kind::MidpointV;
            residual.vars[0] = pv;
            residual.vars[1] = line->startV;
            residual.vars[2] = line->endV;
            out.problem.residuals.push_back(residual);

        } else if (const auto* c = std::get_if<SymmetricConstraint>(&data)) {
            int au = -1, av = -1, bu = -1, bv = -1;
            if (const char* why = resolveRef(c->a, au, av)) { reject(id, why); continue; }
            if (const char* why = resolveRef(c->b, bu, bv)) { reject(id, why); continue; }
            if (c->a.entityId == c->b.entityId && c->a.subElement == c->b.subElement) {
                reject(id, "a point is its own mirror image only on the line itself");
                continue;
            }
            if (c->a.entityId == c->line || c->b.entityId == c->line) {
                reject(id, "a point cannot be mirrored across the line it belongs to");
                continue;
            }
            const EntitySlots* mirror = nullptr;
            if (const char* why = lineSlots(c->line, &mirror)) { reject(id, why); continue; }
            if (const char* why = requireDirected(c->line)) { reject(id, why); continue; }

            // TWO residuals from one constraint. The pair is what symmetry
            // means; either alone admits configurations that are plainly not
            // mirror images.
            residual.vars[0] = au;
            residual.vars[1] = av;
            residual.vars[2] = bu;
            residual.vars[3] = bv;
            residual.vars[4] = mirror->startU;
            residual.vars[5] = mirror->startV;
            residual.vars[6] = mirror->endU;
            residual.vars[7] = mirror->endV;
            residual.kind = SolveResidual::Kind::SymmetricAcross;
            out.problem.residuals.push_back(residual);
            residual.kind = SolveResidual::Kind::SymmetricAlong;
            out.problem.residuals.push_back(residual);

        } else if (const auto* c = std::get_if<PointLineDistanceConstraint>(&data)) {
            int pu = -1, pv = -1;
            if (const char* why = resolveRef(c->point, pu, pv)) { reject(id, why); continue; }
            if (c->point.entityId == c->line) {
                reject(id, "a point is always on the line it belongs to");
                continue;
            }
            const EntitySlots* line = nullptr;
            if (const char* why = lineSlots(c->line, &line)) { reject(id, why); continue; }
            // A zero-length line has no direction, so it has no side and no
            // perpendicular. Refused here rather than dividing by it.
            if (const char* why = requireDirected(c->line)) { reject(id, why); continue; }
            residual.kind = SolveResidual::Kind::PointLineDistance;
            residual.vars[0] = pu;
            residual.vars[1] = pv;
            residual.vars[2] = line->startU;
            residual.vars[3] = line->startV;
            residual.vars[4] = line->endU;
            residual.vars[5] = line->endV;
            residual.target = target;
            out.problem.residuals.push_back(residual);

        } else if (const auto* c = std::get_if<EllipseAxisConstraint>(&data)) {
            const SketchEntity* entity = sketch.findEntity(c->curve);
            if (entity == nullptr) {
                reject(id, "references an entity that is not in this sketch");
                continue;
            }
            if (!std::holds_alternative<SketchEllipse>(entity->geometry) &&
                !std::holds_alternative<SketchEllipticalArc>(entity->geometry)) {
                reject(id, "a major or minor axis dimension needs an ellipse");
                continue;
            }
            const EntitySlots& s2 = slots.at(ToObjectId(c->curve));
            residual.kind = SolveResidual::Kind::Radius;
            residual.vars[0] = c->minor ? s2.minorRadius : s2.radius;
            residual.target = target;
            out.problem.residuals.push_back(residual);

        } else if (const auto* c = std::get_if<EllipseRotationConstraint>(&data)) {
            const SketchEntity* entity = sketch.findEntity(c->curve);
            if (entity == nullptr) {
                reject(id, "references an entity that is not in this sketch");
                continue;
            }
            if (!std::holds_alternative<SketchEllipse>(entity->geometry) &&
                !std::holds_alternative<SketchEllipticalArc>(entity->geometry)) {
                reject(id, "an orientation dimension needs an ellipse");
                continue;
            }
            residual.kind = SolveResidual::Kind::EllipseRotation;
            residual.vars[0] = slots.at(ToObjectId(c->curve)).rotation;
            residual.target = target;
            out.problem.residuals.push_back(residual);

        } else if (const auto* c = std::get_if<PointOnObjectConstraint>(&data)) {
            int pu = -1, pv = -1;
            if (const char* why = resolveRef(c->point, pu, pv)) { reject(id, why); continue; }
            const SketchEntity* target_ = sketch.findEntity(c->target);
            if (target_ == nullptr) {
                reject(id, "references an entity that is not in this sketch");
                continue;
            }
            if (c->point.entityId == c->target) {
                reject(id, "a point is always on the entity it belongs to");
                continue;
            }
            if (std::holds_alternative<SketchLine>(target_->geometry)) {
                const EntitySlots* line = nullptr;
                if (const char* why = lineSlots(c->target, &line)) { reject(id, why); continue; }
                if (const char* why = requireDirected(c->target)) { reject(id, why); continue; }
                residual.kind = SolveResidual::Kind::PointOnLine;
                residual.vars[0] = pu;
                residual.vars[1] = pv;
                residual.vars[2] = line->startU;
                residual.vars[3] = line->startV;
                residual.vars[4] = line->endU;
                residual.vars[5] = line->endV;
                out.problem.residuals.push_back(residual);
            } else if (std::holds_alternative<SketchCircle>(target_->geometry) ||
                       std::holds_alternative<SketchArc>(target_->geometry)) {
                const EntitySlots* curve = nullptr;
                if (const char* why = curveSlots(c->target, &curve)) { reject(id, why); continue; }
                residual.kind = SolveResidual::Kind::PointOnCircle;
                residual.vars[0] = pu;
                residual.vars[1] = pv;
                residual.vars[2] = curve->startU;
                residual.vars[3] = curve->startV;
                residual.vars[4] = curve->radius;
                out.problem.residuals.push_back(residual);
            } else if (std::holds_alternative<SketchEllipse>(target_->geometry) ||
                       std::holds_alternative<SketchEllipticalArc>(target_->geometry)) {
                const EntitySlots& s2 = slots.at(ToObjectId(c->target));
                residual.kind = SolveResidual::Kind::PointOnEllipseImplicit;
                residual.vars[0] = pu;
                residual.vars[1] = pv;
                residual.vars[2] = s2.startU;
                residual.vars[3] = s2.startV;
                residual.vars[4] = s2.radius;
                residual.vars[5] = s2.minorRadius;
                residual.vars[6] = s2.rotation;
                out.problem.residuals.push_back(residual);
            } else {
                reject(id, "point-on-object needs a line, a circle, an arc or an ellipse");
                continue;
            }

        } else {
            const auto& tangent = std::get<TangentConstraint>(data);
            const SketchEntity* entityA = sketch.findEntity(tangent.a);
            const SketchEntity* entityB = sketch.findEntity(tangent.b);
            if (entityA == nullptr || entityB == nullptr) {
                reject(id, "references an entity that is not in this sketch");
                continue;
            }
            if (tangent.a == tangent.b) {
                reject(id, "an entity cannot be tangent to itself");
                continue;
            }
            const auto isCurve = [](const SketchEntity& e) {
                return std::holds_alternative<SketchCircle>(e.geometry) ||
                       std::holds_alternative<SketchArc>(e.geometry);
            };
            const bool curveA = isCurve(*entityA);
            const bool curveB = isCurve(*entityB);
            const bool lineA = std::holds_alternative<SketchLine>(entityA->geometry);
            const bool lineB = std::holds_alternative<SketchLine>(entityB->geometry);
            const bool splineA = std::holds_alternative<SketchSpline>(entityA->geometry);
            const bool splineB = std::holds_alternative<SketchSpline>(entityB->geometry);
            const auto isEllipse = [](const SketchEntity& e) {
                return std::holds_alternative<SketchEllipse>(e.geometry) ||
                       std::holds_alternative<SketchEllipticalArc>(e.geometry);
            };
            const bool ellipseA = isEllipse(*entityA);
            const bool ellipseB = isEllipse(*entityB);

            // TANGENT AT A POINT is a different equation, not a hint -- so
            // anything that cannot say WHERE is refused rather than downgraded
            // to the distance form. Downgrading would put the sketch back in
            // the rank-deficient case this exists to leave, and say nothing.
            const bool atPoint = tangent.at == SketchSubElement::StartPoint ||
                                 tangent.at == SketchSubElement::EndPoint;
            if (tangent.at != SketchSubElement::Whole && !atPoint) {
                reject(id, "tangency can only be pinned at a start or end point");
                continue;
            }
            // `at` NAMES AN END OF `a`, so `a` has to be the entity that has
            // one. A line-curve pair with the curve first cannot say where it
            // touches -- refused rather than quietly applied to the line, which
            // would hold an end the caller never named.
            if (atPoint && curveA && !curveB && !splineB) {
                reject(id, "tangency at a point must name the end on the FIRST entity");
                continue;
            }

            // --- AN ELLIPSE (M18) -----------------------------------------
            //
            // Only against a LINE. A line tangent to an ellipse has a closed
            // form -- h^2 = a^2 nu^2 + b^2 nv^2, with no contact point in it --
            // while a circle, an arc, a spline or a second ellipse needs the
            // touch parameter solved for, and no constraint owns a variable of
            // its own yet.
            //
            // Refused with what is missing rather than approximated. An
            // approximation here would be a tangency that converges and does
            // not touch, which is the failure ADR-M17-044 was about, arriving
            // by a new door.
            if (ellipseA || ellipseB) {
                const bool lineOther = ellipseA ? lineB : lineA;
                if (!lineOther) {
                    reject(id, "an ellipse can only be tangent to a line so far; against a "
                               "circle, an arc or another ellipse the touch point has to be "
                               "solved for");
                    continue;
                }
                if (atPoint) {
                    // WHERE it touches is not a variable here -- the closed
                    // form has no contact point in it -- so naming an end
                    // would promise something this equation cannot hold.
                    reject(id, "tangency to an ellipse cannot be pinned at an end yet");
                    continue;
                }
                const SketchEntityId lineId = ellipseA ? tangent.b : tangent.a;
                const SketchEntityId ellipseId = ellipseA ? tangent.a : tangent.b;
                const EntitySlots* line = nullptr;
                if (const char* why = lineSlots(lineId, &line)) { reject(id, why); continue; }
                if (const char* why = requireDirected(lineId)) { reject(id, why); continue; }
                const EntitySlots& oval = slots.at(ToObjectId(ellipseId));
                if (oval.radius < 0 || oval.minorRadius < 0 || oval.rotation < 0) {
                    reject(id, "that ellipse has no axes to be tangent to");
                    continue;
                }
                residual.kind = SolveResidual::Kind::TangentLineEllipse;
                residual.vars[0] = line->startU;
                residual.vars[1] = line->startV;
                residual.vars[2] = line->endU;
                residual.vars[3] = line->endV;
                residual.vars[4] = oval.startU;
                residual.vars[5] = oval.startV;
                residual.vars[6] = oval.radius;
                residual.vars[7] = oval.minorRadius;
                residual.vars[8] = oval.rotation;
                out.problem.residuals.push_back(residual);
                continue;
            }

            // --- A SPLINE'S END (M18) -------------------------------------
            //
            // Always "at a point", never whole: a spline has a different
            // tangent at every point along it, so "this spline is tangent to
            // that line" does not name an equation. Refused rather than
            // guessing an end, because guessing would hold a point the caller
            // never named -- the same reason the line-curve form above insists
            // on being told which end touches.
            if (splineA || splineB) {
                if (!atPoint) {
                    reject(id, "a spline is tangent AT one of its ends; say which end");
                    continue;
                }
                // The end named by `at` belongs to `a`, as everywhere else in
                // this constraint, so a spline that is meant to supply the end
                // has to be first.
                if (!splineA) {
                    reject(id, "tangency at a spline's end must name the spline FIRST");
                    continue;
                }
                SplineEnd end{};
                if (const char* why = splineEnd(tangent.a, tangent.at, &end)) {
                    reject(id, why);
                    continue;
                }

                if (splineB) {
                    // TWO SPLINES meeting smoothly: both chords point the same
                    // way. `at` names a's end; b's end is whichever of its own
                    // two is the one being joined, and the caller says so the
                    // only way it can -- by having put a Coincident there. We
                    // take b's NEAR end: the one already at a's end.
                    const SketchEntity* other = sketch.findEntity(tangent.b);
                    const auto* otherSpline =
                        other == nullptr ? nullptr : std::get_if<SketchSpline>(&other->geometry);
                    const auto* mine = std::get_if<SketchSpline>(&entityA->geometry);
                    if (otherSpline == nullptr || mine == nullptr || otherSpline->closed ||
                        otherSpline->points.size() < 2 || mine->points.size() < 2) {
                        reject(id, "both splines need two open ends to meet at");
                        continue;
                    }
                    const Vec2 tip = tangent.at == SketchSubElement::StartPoint
                                         ? mine->points.front()
                                         : mine->points.back();
                    const double toStart = std::hypot(otherSpline->points.front().x - tip.x,
                                                      otherSpline->points.front().y - tip.y);
                    const double toEnd = std::hypot(otherSpline->points.back().x - tip.x,
                                                    otherSpline->points.back().y - tip.y);
                    SplineEnd far{};
                    if (const char* why = splineEnd(tangent.b,
                                                    toStart <= toEnd
                                                        ? SketchSubElement::StartPoint
                                                        : SketchSubElement::EndPoint,
                                                    &far)) {
                        reject(id, why);
                        continue;
                    }
                    // PARALLEL CHORDS, in the packing LinesParallel already
                    // reads: two lines, four points. The equation for "these
                    // two leave in the same direction" is the equation for
                    // "these two lines are parallel" -- writing a second
                    // residual that said the same thing would be one more pair
                    // that has to agree.
                    residual.kind = SolveResidual::Kind::LinesParallel;
                    residual.vars[0] = end.endU;
                    residual.vars[1] = end.endV;
                    residual.vars[2] = end.nextU;
                    residual.vars[3] = end.nextV;
                    residual.vars[4] = far.endU;
                    residual.vars[5] = far.endV;
                    residual.vars[6] = far.nextU;
                    residual.vars[7] = far.nextV;
                    out.problem.residuals.push_back(residual);
                    continue;
                }

                if (lineB) {
                    const EntitySlots* line = nullptr;
                    if (const char* why = lineSlots(tangent.b, &line)) { reject(id, why); continue; }
                    if (const char* why = requireDirected(tangent.b)) { reject(id, why); continue; }
                    residual.kind = SolveResidual::Kind::LinesParallel;
                    residual.vars[0] = end.endU;
                    residual.vars[1] = end.endV;
                    residual.vars[2] = end.nextU;
                    residual.vars[3] = end.nextV;
                    residual.vars[4] = line->startU;
                    residual.vars[5] = line->startV;
                    residual.vars[6] = line->endU;
                    residual.vars[7] = line->endV;
                    out.problem.residuals.push_back(residual);
                    continue;
                }

                if (curveB) {
                    const EntitySlots* curve = nullptr;
                    if (const char* why = curveSlots(tangent.b, &curve)) {
                        reject(id, why);
                        continue;
                    }
                    // PERPENDICULAR TO THE RADIUS at the touch point, which is
                    // TangentAtPoint's exact equation -- (touch, far, centre,
                    // radius). The spline's end is the touch point and its
                    // neighbour is the far point, so the chord plays the part
                    // the line played there.
                    residual.kind = SolveResidual::Kind::TangentAtPoint;
                    residual.vars[0] = end.endU;
                    residual.vars[1] = end.endV;
                    residual.vars[2] = end.nextU;
                    residual.vars[3] = end.nextV;
                    residual.vars[4] = curve->startU;
                    residual.vars[5] = curve->startV;
                    residual.vars[6] = curve->radius;
                    out.problem.residuals.push_back(residual);
                    continue;
                }

                reject(id, "a spline's end can be tangent to a line, a circle, an arc or "
                           "another spline");
                continue;
            }

            if (curveA && curveB) {
                const EntitySlots* a = nullptr;
                const EntitySlots* b = nullptr;
                if (const char* why = curveSlots(tangent.a, &a)) { reject(id, why); continue; }
                if (const char* why = curveSlots(tangent.b, &b)) { reject(id, why); continue; }

                if (atPoint) {
                    // WHERE they touch, as a variable. An arc's tips are real
                    // variables (ArcTipU/V), so the point the two share is
                    // something the residual can read -- which is the whole
                    // reason this form exists.
                    const bool atStart = tangent.at == SketchSubElement::StartPoint;
                    const int touchU = atStart ? a->tipStartU : a->tipEndU;
                    const int touchV = atStart ? a->tipStartV : a->tipEndV;
                    if (touchU < 0 || touchV < 0) {
                        // A CIRCLE has no ends. Refused rather than falling back
                        // to the centre-distance form, which would silently be
                        // the rank-deficient constraint again.
                        reject(id, "tangency at a point needs an arc, which has ends, not a "
                                   "circle");
                        continue;
                    }
                    residual.kind = SolveResidual::Kind::TangentCurvesAtPoint;
                    residual.vars[0] = touchU;
                    residual.vars[1] = touchV;
                    residual.vars[2] = a->startU;
                    residual.vars[3] = a->startV;
                    residual.vars[4] = b->startU;
                    residual.vars[5] = b->startV;
                    residual.vars[6] = a->radius;
                    residual.vars[7] = b->radius;
                    out.problem.residuals.push_back(residual);
                    continue;
                }

                // The branch is READ from the constraint, never re-derived from
                // the current configuration: a solve that re-guessed it would
                // let a drag silently swap "outside each other" for "one inside
                // the other", which is a different model, not a different pose.
                residual.kind = tangent.internal ? SolveResidual::Kind::TangentCirclesInner
                                                 : SolveResidual::Kind::TangentCirclesOuter;
                residual.vars[0] = a->startU;
                residual.vars[1] = a->startV;
                residual.vars[2] = a->radius;
                residual.vars[3] = b->startU;
                residual.vars[4] = b->startV;
                residual.vars[5] = b->radius;
                out.problem.residuals.push_back(residual);
            } else if ((lineA && curveB) || (curveA && lineB)) {
                const SketchEntityId lineId = lineA ? tangent.a : tangent.b;
                const SketchEntityId curveId = lineA ? tangent.b : tangent.a;
                const EntitySlots* line = nullptr;
                const EntitySlots* curve = nullptr;
                if (const char* why = lineSlots(lineId, &line)) { reject(id, why); continue; }
                if (const char* why = curveSlots(curveId, &curve)) { reject(id, why); continue; }
                // Both residuals divide by the line's length.
                if (const char* why = requireDirected(lineId)) { reject(id, why); continue; }
                // WHICH END TOUCHES decides the packing, and the packing is the
                // whole constraint: the touching end goes first, the other end
                // second. Both are U/V slots, so nothing downstream can tell
                // them apart if they are swapped -- the tangency would hold the
                // far corner square and let the near one kink, which is a
                // wrong answer that still converges.
                // Asked as "is it the END", so the unspecified case keeps the
                // start-first packing every existing tangency already has.
                const bool touchAtEnd = tangent.at == SketchSubElement::EndPoint;
                residual.kind = atPoint ? SolveResidual::Kind::TangentAtPoint
                                        : SolveResidual::Kind::TangentLineCircle;
                residual.vars[0] = touchAtEnd ? line->endU : line->startU;
                residual.vars[1] = touchAtEnd ? line->endV : line->startV;
                residual.vars[2] = touchAtEnd ? line->startU : line->endU;
                residual.vars[3] = touchAtEnd ? line->startV : line->endV;
                residual.vars[4] = curve->startU;
                residual.vars[5] = curve->startV;
                residual.vars[6] = curve->radius;
                out.problem.residuals.push_back(residual);
            } else {
                reject(id, "Tangent needs a line and a curve, or two curves");
                continue;
            }
        }
    }

    return out;
}

bool CommitSolvedGeometry(Sketch& sketch, const SketchSolveProblem& problem,
                          const SketchSolveResult& result) {
    if (result.values.size() != problem.variables.size()) return false;

    std::vector<std::pair<SketchEntityId, SketchGeometry>> pending;

    // Gather each entity's solved scalars first, then write whole entities.
    // Writing scalar by scalar would leave an entity half-updated if anything
    // went wrong partway, which is the shape of defect ADR-M5-004 exists to
    // prevent.
    // Routed by SUB-ELEMENT, then interpreted per entity type.
    //
    // A line's StartPoint and an arc's StartPoint are both "U/V at StartPoint",
    // and they mean completely different things -- an endpoint the solver owns
    // versus a tip DERIVED from the centre, radius and angle. Folding them into
    // one pair of fields, as an earlier version did, made an arc's tip
    // overwrite its centre.
    struct Gathered {
        double wholeU{0}, wholeV{0};
        double startU{0}, startV{0}, endU{0}, endV{0};
        double centreU{0}, centreV{0};
        double radius{0}, startAngle{0}, endAngle{0};
        double minorRadius{0}, rotation{0};
        bool hasWhole{false}, hasStart{false}, hasEnd{false}, hasCentre{false};
        bool hasRadius{false}, hasStartAngle{false}, hasEndAngle{false};
        bool hasMinorRadius{false}, hasRotation{false};
        // A SPLINE'S POINTS, in variable order. Gathered separately from the
        // sub-element routing below because a spline's interior points all
        // carry Whole and would otherwise overwrite one another.
        std::vector<Vec2> splinePoints;
    };
    std::map<ObjectId, Gathered> gathered;

    for (std::size_t i = 0; i < problem.variables.size(); ++i) {
        const SolveVariable& v = problem.variables[i];
        const double value = result.values[i];
        if (!std::isfinite(value)) return false; // never commit a non-finite value
        Gathered& g = gathered[ToObjectId(v.entityId)];

        // A SPLINE IS GATHERED BY POSITION, not by sub-element.
        //
        // Its interior points all carry Whole, so the routing below would
        // collapse every one of them onto the same pair of fields and write a
        // spline whose middle had been replaced by its last point. The
        // variables were made in point order, u then v, so appending in that
        // order rebuilds the list exactly -- and the write-back checks the
        // count before trusting it.
        const SketchEntity* owner = sketch.findEntity(v.entityId);
        if (owner != nullptr && std::holds_alternative<SketchSpline>(owner->geometry)) {
            if (v.component == SolveVariable::Component::U)
                g.splinePoints.push_back(Vec2{value, 0.0});
            else if (v.component == SolveVariable::Component::V && !g.splinePoints.empty())
                g.splinePoints.back().y = value;
            continue;
        }

        switch (v.component) {
            case SolveVariable::Component::Radius:
                g.radius = value;
                g.hasRadius = true;
                break;
            case SolveVariable::Component::MinorRadius:
                g.minorRadius = value;
                g.hasMinorRadius = true;
                break;
            case SolveVariable::Component::Rotation:
                g.rotation = value;
                g.hasRotation = true;
                break;
            // An ellipse's two ends carry PARAMETERS rather than angles, and
            // they land in the same fields: what differs is the number's
            // meaning to the geometry, and the visit below is where that is
            // interpreted.
            case SolveVariable::Component::StartParam:
            case SolveVariable::Component::StartAngle:
                g.startAngle = value;
                g.hasStartAngle = true;
                break;
            case SolveVariable::Component::EndParam:
            case SolveVariable::Component::EndAngle:
                g.endAngle = value;
                g.hasEndAngle = true;
                break;
            case SolveVariable::Component::U:
            case SolveVariable::Component::V: {
                const bool isU = v.component == SolveVariable::Component::U;
                switch (v.subElement) {
                    case SketchSubElement::EndPoint:
                        (isU ? g.endU : g.endV) = value;
                        g.hasEnd = true;
                        break;
                    case SketchSubElement::StartPoint:
                        (isU ? g.startU : g.startV) = value;
                        g.hasStart = true;
                        break;
                    case SketchSubElement::CenterPoint:
                        (isU ? g.centreU : g.centreV) = value;
                        g.hasCentre = true;
                        break;
                    case SketchSubElement::Whole:
                        (isU ? g.wholeU : g.wholeV) = value;
                        g.hasWhole = true;
                        break;
                }
                break;
            }
        }
    }

    for (const auto& [entityId, g] : gathered) {
        const SketchEntity* existing = sketch.findEntity(static_cast<SketchEntityId>(entityId));
        if (existing == nullptr) continue;
        SketchGeometry updated = existing->geometry;
        std::visit(
            [&](auto& geometry) {
                using T = std::decay_t<decltype(geometry)>;
                if constexpr (std::is_same_v<T, SketchPoint>) {
                    if (g.hasWhole) geometry.position = Vec2{g.wholeU, g.wholeV};
                } else if constexpr (std::is_same_v<T, SketchLine>) {
                    if (g.hasStart) geometry.start = Vec2{g.startU, g.startV};
                    if (g.hasEnd) geometry.end = Vec2{g.endU, g.endV};
                } else if constexpr (std::is_same_v<T, SketchCircle>) {
                    if (g.hasCentre) geometry.center = Vec2{g.centreU, g.centreV};
                    if (g.hasRadius) geometry.radiusMm = g.radius;
                } else if constexpr (std::is_same_v<T, SketchSpline>) {
                    // WRITTEN BACK BY INDEX, from a list gathered in the same
                    // order the variables were made. The other kinds route by
                    // sub-element, which cannot work here: a spline's interior
                    // points all carry Whole, so routing by sub-element would
                    // collapse every one of them onto the same field.
                    if (g.splinePoints.size() == geometry.points.size())
                        geometry.points = g.splinePoints;
                } else if constexpr (std::is_same_v<T, SketchEllipse> ||
                                     std::is_same_v<T, SketchEllipticalArc>) {
                    if (g.hasCentre) geometry.center = Vec2{g.centreU, g.centreV};
                    if (g.hasRadius) geometry.majorRadiusMm = g.radius;
                    if (g.hasMinorRadius) geometry.minorRadiusMm = g.minorRadius;
                    if (g.hasRotation) geometry.rotationRad = g.rotation;
                    if constexpr (std::is_same_v<T, SketchEllipticalArc>) {
                        // The PARAMETERS are the state; the tips are what
                        // EllipseTipU/V derive from them. Writing the solved
                        // tips back would store the same fact twice, in two
                        // places that can then disagree.
                        if (g.hasStartAngle) geometry.startParamRad = g.startAngle;
                        if (g.hasEndAngle) geometry.endParamRad = g.endAngle;
                    }
                } else {
                    static_assert(std::is_same_v<T, SketchArc>);
                    if (g.hasCentre) geometry.center = Vec2{g.centreU, g.centreV};
                    if (g.hasRadius) geometry.radiusMm = g.radius;
                    // The ANGLES are the state; the tips are what ArcTipU/V
                    // derive FROM them. Writing the solved tip coordinates back
                    // would be storing the same fact twice, in two places that
                    // can then disagree.
                    if (g.hasStartAngle) geometry.startAngleRad = g.startAngle;
                    if (g.hasEndAngle) geometry.endAngleRad = g.endAngle;
                }
            },
            updated);

        // VALIDATED, not just finite -- but ONLY what this commit is about to
        // write, and only when the solve actually changed it.
        //
        // Validating every entity in the sketch made one pre-existing
        // degenerate entity poison the whole sketch permanently: restoreEntity
        // deliberately does not validate (a hand-edited file must round-trip),
        // so a loaded document containing one such entity could never solve
        // again, with a diagnostic naming no constraint and a stale DOF of 0
        // reading as "fully constrained". A commit is answerable for what it
        // writes, not for what it found.
        const SketchEntity* before = sketch.findEntity(static_cast<SketchEntityId>(entityId));
        if (before != nullptr && !SameSketchGeometryValue(before->geometry, updated)) {
            if (!IsValidSketchGeometry(updated)) return false;
        }
        pending.emplace_back(static_cast<SketchEntityId>(entityId), std::move(updated));
    }

    for (auto& [entityId, geometry] : pending)
        sketch.replaceGeometry(entityId, std::move(geometry));
    return true;
}

} // namespace paramcad
