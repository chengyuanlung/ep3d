#include "Core/Sketch/SketchSolveSession.h"

#include "Core/Document/ObjectRegistry.h"
#include "Core/Parameter/Parameter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
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
};

const Parameter* ResolveParameter(const ObjectRegistry& registry, ObjectId id) {
    if (id == kInvalidObjectId) return nullptr;
    const ObjectRegistry::ObjectRef* ref = registry.find(id);
    if (ref == nullptr) return nullptr;
    auto* const* parameter = std::get_if<Parameter*>(ref);
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
    if (std::holds_alternative<AngleConstraint>(data)) return unit == UnitType::Radian;
    return unit == UnitType::Millimeter;
}

bool DimensionValueValid(const SketchConstraintData& data, double value) noexcept {
    if (!std::isfinite(value)) return false;
    // Angles may be any finite value; lengths, distances and radii must be
    // strictly positive (ADR-M5-002: a zero-length line has no direction, a
    // zero-radius circle is a point).
    if (std::holds_alternative<AngleConstraint>(data)) return true;
    return value >= kMinSketchDimensionMm;
}

// Whether the solve actually moved this entity. An unchanged entity is not
// this commit's business: it was already in the sketch, valid or not.
bool SameSketchGeometry(const SketchGeometry& a, const SketchGeometry& b) noexcept {
    if (a.index() != b.index()) return false;
    const auto same = [](double x, double y) { return x == y; };
    if (const auto* pa = std::get_if<SketchPoint>(&a)) {
        const auto& pb = std::get<SketchPoint>(b);
        return same(pa->position.x, pb.position.x) && same(pa->position.y, pb.position.y);
    }
    if (const auto* la = std::get_if<SketchLine>(&a)) {
        const auto& lb = std::get<SketchLine>(b);
        return same(la->start.x, lb.start.x) && same(la->start.y, lb.start.y) &&
               same(la->end.x, lb.end.x) && same(la->end.y, lb.end.y);
    }
    if (const auto* ca = std::get_if<SketchCircle>(&a)) {
        const auto& cb = std::get<SketchCircle>(b);
        return same(ca->center.x, cb.center.x) && same(ca->center.y, cb.center.y) &&
               same(ca->radiusMm, cb.radiusMm);
    }
    const auto& aa = std::get<SketchArc>(a);
    const auto& ab = std::get<SketchArc>(b);
    return same(aa.center.x, ab.center.x) && same(aa.center.y, ab.center.y) &&
           same(aa.radiusMm, ab.radiusMm) && same(aa.startAngleRad, ab.startAngleRad) &&
           same(aa.endAngleRad, ab.endAngleRad) && aa.counterClockwise == ab.counterClockwise;
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
            bool wantU) {
    if (std::holds_alternative<SketchLine>(entity.geometry)) {
        // A line has two endpoints and nothing else. "Whole" is ambiguous for
        // it, and it has no centre.
        if (part == SketchSubElement::StartPoint) return wantU ? slots.startU : slots.startV;
        if (part == SketchSubElement::EndPoint) return wantU ? slots.endU : slots.endV;
        return -1;
    }
    if (std::holds_alternative<SketchCircle>(entity.geometry) ||
        std::holds_alternative<SketchArc>(entity.geometry)) {
        // A curve's only referenceable point is its centre. Its start and end
        // are functions of the radius and the angles, not independent state, so
        // naming one is a modelling error rather than a shorthand for anything.
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
                } else {
                    static_assert(std::is_same_v<T, SketchArc>);
                    // An arc's centre and radius are solved; its angular extent
                    // is held fixed in M5. Documented rather than silent: the
                    // constraints M5 ships (spec 6) do not drive arc endpoints,
                    // so making the angles variables would add free DOF nothing
                    // can remove and every arc sketch would read as
                    // under-constrained.
                    s.startU = addVariable(entity->id, SketchSubElement::CenterPoint,
                                           SolveVariable::Component::U, geometry.center.x);
                    s.startV = addVariable(entity->id, SketchSubElement::CenterPoint,
                                           SolveVariable::Component::V, geometry.center.y);
                    s.radius = addVariable(entity->id, SketchSubElement::Whole,
                                           SolveVariable::Component::Radius, geometry.radiusMm);
                }
            },
            entity->geometry);
        slots[ToObjectId(entity->id)] = s;
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
        u = SlotFor(it->second, *entity, ref.subElement, true);
        v = SlotFor(it->second, *entity, ref.subElement, false);
        if (u < 0 || v < 0) return "names a sub-element this entity does not have";
        return nullptr;
    };

    for (const SketchConstraint* constraint : constraints) {
        const SketchConstraintData& data = constraint->data;
        const SketchConstraintId id = constraint->id;

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
        const auto curveSlots = [&](SketchEntityId curveId,
                                    const EntitySlots** found) -> const char* {
            const SketchEntity* entity = sketch.findEntity(curveId);
            if (entity == nullptr) return "references an entity that is not in this sketch";
            if (!std::holds_alternative<SketchCircle>(entity->geometry) &&
                !std::holds_alternative<SketchArc>(entity->geometry))
                return "requires a circle or an arc";
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

        } else {
            const auto& angle = std::get<AngleConstraint>(data);
            const EntitySlots* a = nullptr;
            const EntitySlots* b = nullptr;
            if (const char* why = lineSlots(angle.lineA, &a)) { reject(id, why); continue; }
            if (const char* why = lineSlots(angle.lineB, &b)) { reject(id, why); continue; }
            // ADR-M5-006: an angle needs a DIRECTION, and a line shorter than
            // kMinSketchDimensionMm has none. Without this the residual is
            // atan2(0, 0) and the solver pulls the degenerate line's endpoints
            // apart to satisfy an angle that was never defined -- it silently
            // edits geometry the user did not ask it to touch. addLine already
            // rejects degenerate geometry, but restoreEntity deliberately does
            // not (a hand-edited file must round-trip), so the check has to
            // live here, where the constraint is actually used.
            if (const char* why = requireDirected(angle.lineA)) { reject(id, why); continue; }
            if (const char* why = requireDirected(angle.lineB)) { reject(id, why); continue; }
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
    struct Gathered {
        double startU{0}, startV{0}, endU{0}, endV{0}, radius{0};
        bool hasStart{false}, hasEnd{false}, hasRadius{false};
    };
    std::map<ObjectId, Gathered> gathered;

    for (std::size_t i = 0; i < problem.variables.size(); ++i) {
        const SolveVariable& v = problem.variables[i];
        const double value = result.values[i];
        if (!std::isfinite(value)) return false; // never commit a non-finite value
        Gathered& g = gathered[ToObjectId(v.entityId)];
        switch (v.component) {
            case SolveVariable::Component::Radius:
                g.radius = value;
                g.hasRadius = true;
                break;
            case SolveVariable::Component::U:
                if (v.subElement == SketchSubElement::EndPoint) {
                    g.endU = value;
                    g.hasEnd = true;
                } else {
                    g.startU = value;
                    g.hasStart = true;
                }
                break;
            case SolveVariable::Component::V:
                if (v.subElement == SketchSubElement::EndPoint) {
                    g.endV = value;
                    g.hasEnd = true;
                } else {
                    g.startV = value;
                    g.hasStart = true;
                }
                break;
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
                    if (g.hasStart) geometry.position = Vec2{g.startU, g.startV};
                } else if constexpr (std::is_same_v<T, SketchLine>) {
                    if (g.hasStart) geometry.start = Vec2{g.startU, g.startV};
                    if (g.hasEnd) geometry.end = Vec2{g.endU, g.endV};
                } else if constexpr (std::is_same_v<T, SketchCircle>) {
                    if (g.hasStart) geometry.center = Vec2{g.startU, g.startV};
                    if (g.hasRadius) geometry.radiusMm = g.radius;
                } else {
                    static_assert(std::is_same_v<T, SketchArc>);
                    if (g.hasStart) geometry.center = Vec2{g.startU, g.startV};
                    if (g.hasRadius) geometry.radiusMm = g.radius;
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
        if (before != nullptr && !SameSketchGeometry(before->geometry, updated)) {
            if (!IsValidSketchGeometry(updated)) return false;
        }
        pending.emplace_back(static_cast<SketchEntityId>(entityId), std::move(updated));
    }

    for (auto& [entityId, geometry] : pending)
        sketch.replaceGeometry(entityId, std::move(geometry));
    return true;
}

} // namespace paramcad
