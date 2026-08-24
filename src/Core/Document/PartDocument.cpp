#include "Core/Document/PartDocument.h"

#include "Core/Expression/ExpressionEvaluator.h"
#include "Core/Feature/BoxFeature.h"
#include "Core/Feature/IMaterialReferencing.h"
#include "Core/Feature/ISketchConsuming.h"
#include "Core/Feature/FeatureSnapshot.h"
#include "Core/Geometry/Transform.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/PlaceholderFeature.h"
#include "Core/Feature/PocketFeature.h"
#include "Core/Feature/BooleanFeature.h"
#include "Core/Feature/DraftFeature.h"
#include "Core/Feature/HoleFeature.h"
#include "Core/Feature/ImportFeature.h"
#include "Core/Feature/LoftFeature.h"
#include "Core/Feature/ShellFeature.h"
#include "Core/Feature/RevolveFeature.h"
#include "Core/Feature/SweepFeature.h"
#include "Core/Feature/EdgeDressFeatures.h"
#include "Core/Recompute/IRecomputable.h"
#include "Core/Sketch/SketchSolveSession.h"
#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace paramcad {

namespace {

// Makes a facade call that records SEVERAL deltas into ONE undo step.
//
// `recordDelta` with no transaction open pushes one RECORD per delta, so a
// call that removes a constraint and its placement produced two undo steps and
// a single Ctrl+Z brought back the constraint without its position. Every
// caller in the shell already wrapped these in a transaction, which is exactly
// why it went unnoticed: the defect only shows when the facade is used
// directly -- which is what a test does.
//
// Adopts an already-open transaction rather than nesting, so a caller that
// wrapped a whole command still gets one step for the whole command.
class ScopedTransaction {
public:
    ScopedTransaction(PartDocument& document, std::string label) : document_(document) {
        owned_ = !document.isTransactionOpen();
        if (owned_) document.beginTransaction(std::move(label));
    }
    ScopedTransaction(const ScopedTransaction&) = delete;
    ScopedTransaction& operator=(const ScopedTransaction&) = delete;

    bool commit() {
        if (!owned_) return true;
        owned_ = false;
        return document_.commitTransaction();
    }
    // Anything not committed is ROLLED BACK: a facade call that bails out
    // halfway must not leave half of its deltas behind.
    ~ScopedTransaction() {
        if (owned_) document_.abortTransaction();
    }

private:
    PartDocument& document_;
    bool owned_ = false;
};

// Moves ONE point of a geometry to `targetMm`, leaving the rest of it alone.
//
// Which point is named by the sub-element, in the same vocabulary the solver
// uses -- so a reference the solver has no variable for (an arc's tip,
// ADR-M12-003) is refused here rather than silently moving something else that
// happens to be nearby.
bool SeedDraggedPoint(SketchGeometry& geometry, SketchSubElement part, Vec2 targetMm) noexcept {
    if (auto* point = std::get_if<SketchPoint>(&geometry)) {
        if (part != SketchSubElement::Whole) return false;
        point->position = targetMm;
        return true;
    }
    if (auto* line = std::get_if<SketchLine>(&geometry)) {
        if (part == SketchSubElement::StartPoint) {
            line->start = targetMm;
            return true;
        }
        if (part == SketchSubElement::EndPoint) {
            line->end = targetMm;
            return true;
        }
        return false;
    }
    // EVERY CLOSED-OR-CURVED KIND: the CENTRE only. A curve's tips are
    // functions of its centre, radii and angles rather than state of their own,
    // so there is nothing there to drag.
    //
    // A visit rather than the get_if chain this used to be, whose last line was
    // an unguarded `std::get<SketchArc>` -- when the variant grew two more
    // alternatives that became a bad_variant_access on any drag of an ellipse,
    // through a path with no try/catch above it.
    if (part != SketchSubElement::CenterPoint) return false;
    return std::visit(
        [targetMm](auto& value) -> bool {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, SketchCircle> || std::is_same_v<T, SketchArc> ||
                          std::is_same_v<T, SketchEllipse> ||
                          std::is_same_v<T, SketchEllipticalArc>) {
                value.center = targetMm;
                return true;
            } else {
                // Point, Line and Spline are handled above or have no centre. A
                // spline's shape IS its points, so there is no single handle
                // that moves the whole of it -- Transform is the command for
                // that, and it moves every point together.
                return false;
            }
        },
        geometry);
}

// The placement a sketch currently holds for a constraint, as an
// (has-a-value, value) pair -- which is what the undo record needs and what a
// bare pointer cannot express once it has been invalidated.
std::pair<bool, Vec2> CurrentPlacement(const Sketch& sketch, SketchConstraintId constraintId) {
    const Vec2* found = sketch.dimensionPlacement(constraintId);
    return found != nullptr ? std::make_pair(true, *found) : std::make_pair(false, Vec2{});
}

} // namespace


PartDocument::PartDocument(std::string name)
    : DocumentBase(std::move(name)) {
    createOriginFrame();
    // MassPropertiesNode is auto-created fresh and auto-registered in the
    // registry (never persisted, ADR-M3-005 -- exactly like the Origin
    // frame), so it is always resolvable. It only JOINS THE GRAPH once
    // wireBoxFeature (via addBoxFeature/restoreBoxFeature) actually gives it
    // a source -- a document with no BoxFeature must not carry a permanently
    // Dirty, permanently failing, edge-less recompute node.
    registry_.registerObject(massPropertiesNode_.id(), &massPropertiesNode_);
}

PartDocument::PartDocument(ObjectId id, std::string name)
    : DocumentBase(id, std::move(name)) {
    createOriginFrame();
    registry_.registerObject(massPropertiesNode_.id(), &massPropertiesNode_);
}

Parameter& PartDocument::addParameter(std::string name, double value, UnitType unit) {
    Parameter& parameter = parameters_.add(std::move(name), value, unit);
    registry_.registerObject(parameter.id(), &parameter);
    graph_.addNode(parameter.id());
    // Recorded HERE and not in restoreParameter, which is the mistake the first
    // version made: deserialization is not a user edit, and a loaded document
    // arrived carrying a history of its own construction. Caught immediately by
    // GATE_G2 and M9_UNDO_402 -- the "a loaded document starts empty" rule
    // earning its place twice over.
    ParameterExistenceEdit edit;
    edit.parameterId = parameter.id();
    edit.name = parameter.name();
    edit.value = parameter.value();
    edit.unit = static_cast<int>(parameter.unit());
    edit.expression = parameter.expression();
    edit.addedByTheEdit = true;
    recordDelta(edit, "Add " + parameter.name());
    return parameter;
}

Parameter& PartDocument::restoreParameter(ObjectId id, std::string name, double value,
                                          UnitType unit, std::string expression,
                                          ParameterState state) {
    // Rejected BEFORE anything is stored. Storing first and throwing afterwards
    // left the duplicate Parameter in ParameterManager, and the document then
    // saved cleanly and could never be loaded back ("duplicate ObjectId").
    // Adding the check without adding the rollback replaced one silent failure
    // with another.
    requireUnusedId(id, "restoreParameter");

    Parameter& parameter =
        parameters_.restore(id, std::move(name), value, unit, std::move(expression), state);
    if (!registry_.registerObject(parameter.id(), &parameter)) {
        parameters_.remove(parameter.id());
        throw std::runtime_error("restoreParameter: id " + std::to_string(id) +
                                 " could not be registered");
    }
    graph_.addNode(parameter.id()); // starts Dirty; graph states are not persisted
    return parameter;
}

bool PartDocument::setParameterValue(ObjectId id, double value) {
    ObjectRegistry::ObjectRef* ref = registry_.find(id);
    if (ref == nullptr) return false;
    auto* const* parameter = std::get_if<Parameter*>(ref);
    if (parameter == nullptr) return false;
    // Recorded BEFORE the write, and only if it changes something: an edit
    // that sets the value it already has is not a step a user should have to
    // undo twice to get past (M9.1).
    ParameterValueEdit edit;
    edit.parameterId = id;
    edit.before = (*parameter)->value();
    edit.after = value;
    edit.expressionBefore = (*parameter)->expression();
    // A TYPED NUMBER REPLACES A FORMULA (M11.2).
    //
    // The alternative -- refuse, and make the user clear the expression first --
    // was rejected because undo replays this exact pair: applyDelta sets the
    // value and THEN the expression, so a rule refusing a value while an
    // expression is present would make undo unable to restore its own record.
    // Clearing keeps one edit atomic in both directions, which is why
    // ParameterValueEdit has carried both fields since M9.1.
    edit.expressionAfter.clear();
    if (!edit.expressionBefore.empty()) {
        detachExpressionEdges(id, edit.expressionBefore);
        (*parameter)->setExpression(std::string{});
    }
    (*parameter)->setValue(value); // ParameterState -> Dirty
    graph_.markDirty(id);          // propagate to dependents
    syncFeatureStatesFromGraph();
    if (edit.before != edit.after || edit.expressionBefore != edit.expressionAfter)
        recordDelta(edit, "Change " + (*parameter)->name());
    return true;
}

bool PartDocument::setParameterExpression(ObjectId id, std::string expression,
                                         ExpressionError* error) {
    if (error != nullptr) *error = ExpressionError{};
    const auto refuse = [error](ExpressionErrorCode code, std::size_t position,
                                std::size_t length, std::string message) {
        if (error != nullptr)
            *error = ExpressionError{code, position, length, std::move(message)};
        return false;
    };

    ObjectRegistry::ObjectRef* ref = registry_.find(id);
    if (ref == nullptr) return false;
    auto* const* slot = std::get_if<Parameter*>(ref);
    if (slot == nullptr) return false;
    Parameter* parameter = *slot;

    const std::string previous = parameter->expression();

    // --- clearing -----------------------------------------------------------
    // Whitespace-only counts, and is NORMALISED to empty, so "  " and "" cannot
    // become two indistinguishable states that round-trip differently.
    if (expression.find_first_not_of(" \t\r\n") == std::string::npos) {
        if (previous.empty()) {
            // Nothing changes. Recording here would cost the user an undo step
            // that undoes nothing -- the rule setParameterValue already follows.
            return true;
        }
        ParameterValueEdit edit;
        edit.parameterId = id;
        edit.before = parameter->value();
        edit.after = parameter->value();
        edit.expressionBefore = previous;
        edit.expressionAfter.clear();
        detachExpressionEdges(id, previous);
        parameter->setExpression(std::string{});
        graph_.markDirty(id);
        syncFeatureStatesFromGraph();
        recordDelta(edit, "Clear " + parameter->name() + " expression");
        return true;
    }

    // --- the field must have a dimension an expression can produce ----------
    const std::optional<Dimension> dimension = ExpressionDimensionOf(parameter->unit());
    if (!dimension.has_value())
        return refuse(ExpressionErrorCode::DimensionMismatch, 0, expression.size(),
                      "parameter '" + parameter->name() +
                          "' has a unit that takes a literal value only, not an expression");

    // --- parse --------------------------------------------------------------
    ExpressionParseResult parsed = ParseExpression(expression);
    if (!parsed) {
        if (error != nullptr) *error = parsed.error;
        return false;
    }

    // --- names: ambiguity, self-reference, and units with no dimension ------
    //
    // Checked BEFORE the trial evaluation, because a resolver can only answer
    // "no value" and these three failures need three different messages. Only
    // genuinely-unknown names are left to the evaluator, which reports them
    // with a POSITION.
    std::vector<ObjectId> prerequisites;
    for (const std::string& name : parsed.expression.referencedVariables()) {
        // First occurrence of "#name" in the source, for the caret. The parser
        // does not carry positions out with the name list, and searching is
        // exact here because a name is preceded by nothing else.
        std::size_t at = expression.find("#" + name);
        if (at == std::string::npos) at = 0;
        const std::size_t span = name.size() + 1;

        bool ambiguous = false;
        const Parameter* referenced = findParameterByExpressionName(name, ambiguous);
        if (ambiguous)
            return refuse(ExpressionErrorCode::UnknownVariable, at, span,
                          "two parameters are named '" + name +
                              "'; rename one, so a reference can only mean one of them");
        if (referenced == nullptr) continue; // the evaluator reports it, with a position
        if (referenced->id() == id)
            return refuse(ExpressionErrorCode::UnknownVariable, at, span,
                          "'" + name + "' is this parameter; an expression cannot read itself");
        if (!ExpressionDimensionOf(referenced->unit()).has_value())
            return refuse(ExpressionErrorCode::DimensionMismatch, at, span,
                          "'" + name + "' has a unit with no expression dimension, so it "
                                       "cannot be used in one");
        prerequisites.push_back(referenced->id());
    }

    // --- trial evaluation ---------------------------------------------------
    //
    // Against the CURRENT values, and it must succeed before the expression is
    // stored. An expression that cannot be evaluated today would leave its
    // parameter Failed on the very next recompute, and the user would discover
    // that separately from the edit that caused it.
    const VariableResolver resolver = [this](std::string_view name) {
        return resolveExpressionVariable(name);
    };
    const ExpressionEvalResult trial =
        EvaluateExpressionForField(parsed.expression, resolver, *dimension);
    if (!trial) {
        if (error != nullptr) *error = trial.error;
        return false;
    }

    // --- edges --------------------------------------------------------------
    //
    // Detach the OLD set, attach the new, and roll BOTH back on refusal, so a
    // rejected edit leaves the graph exactly as it was. Without the rollback, a
    // cycle refusal would silently drop the edges the previous, working
    // expression depended on.
    const std::vector<ObjectId> previousEdges = expressionPrerequisites(previous);
    for (ObjectId prerequisite : previousEdges) graph_.removeDependency(id, prerequisite);

    std::vector<ObjectId> attached;
    for (ObjectId prerequisite : prerequisites) {
        if (graph_.addDependency(id, prerequisite)) {
            attached.push_back(prerequisite);
            continue;
        }
        for (ObjectId done : attached) graph_.removeDependency(id, done);
        for (ObjectId old : previousEdges) graph_.addDependency(id, old);
        std::string path = describeDependencyPath(id, prerequisite);
        if (path.empty()) path = parameter->name();
        return refuse(ExpressionErrorCode::UnknownVariable, 0, expression.size(),
                      "this expression would create a dependency cycle: " + path + " -> " +
                          parameter->name());
    }

    ParameterValueEdit edit;
    edit.parameterId = id;
    edit.before = parameter->value();
    edit.after = parameter->value(); // an expression edit does not move the value; recompute does
    edit.expressionBefore = previous;
    edit.expressionAfter = expression;
    parameter->setExpression(std::move(expression)); // ParameterState -> Dirty
    graph_.markDirty(id);                            // propagate to dependents
    syncFeatureStatesFromGraph();
    if (edit.expressionBefore != edit.expressionAfter)
        recordDelta(edit, "Change " + parameter->name() + " expression");
    return true;
}

// --- Expression plumbing (M11.2) --------------------------------------------

std::vector<std::string> PartDocument::expressionVariableNames(const std::string& text) {
    if (text.empty()) return {};
    ExpressionParseResult parsed = ParseExpression(text);
    if (!parsed) return {}; // deliberately silent; see the header
    return parsed.expression.referencedVariables();
}

const Parameter* PartDocument::findParameterByExpressionName(const std::string& name,
                                                             bool& ambiguous) const {
    ambiguous = false;
    const Parameter* found = nullptr;
    for (const std::unique_ptr<Parameter>& candidate : parameters_.items()) {
        if (candidate->name() != name) continue;
        if (found != nullptr) {
            ambiguous = true;
            return nullptr;
        }
        found = candidate.get();
    }
    return found;
}

std::optional<Quantity> PartDocument::resolveExpressionVariable(std::string_view name) const {
    bool ambiguous = false;
    const Parameter* parameter = findParameterByExpressionName(std::string(name), ambiguous);
    if (parameter == nullptr || ambiguous) return std::nullopt;
    const std::optional<Dimension> dimension = ExpressionDimensionOf(parameter->unit());
    if (!dimension.has_value()) return std::nullopt;
    return Quantity{parameter->value(), *dimension};
}

std::vector<ObjectId> PartDocument::expressionPrerequisites(const std::string& text) const {
    std::vector<ObjectId> ids;
    for (const std::string& name : expressionVariableNames(text)) {
        bool ambiguous = false;
        const Parameter* referenced = findParameterByExpressionName(name, ambiguous);
        if (referenced != nullptr && !ambiguous) ids.push_back(referenced->id());
    }
    return ids;
}

void PartDocument::detachExpressionEdges(ObjectId parameterId, const std::string& text) {
    for (ObjectId prerequisite : expressionPrerequisites(text))
        graph_.removeDependency(parameterId, prerequisite);
}

std::string PartDocument::describeDependencyPath(ObjectId from, ObjectId to) const {
    const auto nameOf = [this](ObjectId id) -> std::string {
        if (const Parameter* parameter = parameters_.findById(id)) return parameter->name();
        return std::to_string(id);
    };
    if (from == to) return nameOf(from);

    // Breadth-first along the DEPENDENTS direction -- the same direction the
    // graph itself checks with `reaches`. BOUNDED by the node count: a walk over
    // a structure that is supposed to be acyclic must not depend on it actually
    // being acyclic in order to terminate.
    std::unordered_map<ObjectId, ObjectId> cameFrom;
    cameFrom[from] = kInvalidObjectId;
    std::vector<ObjectId> frontier{from};
    const std::size_t bound = graph_.nodeCount() + 1;
    for (std::size_t step = 0; step < bound && !frontier.empty(); ++step) {
        std::vector<ObjectId> next;
        for (ObjectId id : frontier) {
            for (ObjectId dependent : graph_.dependentsOf(id)) {
                if (cameFrom.count(dependent) != 0) continue;
                cameFrom[dependent] = id;
                if (dependent == to) {
                    std::vector<ObjectId> path;
                    for (ObjectId walk = to; walk != kInvalidObjectId; walk = cameFrom[walk])
                        path.push_back(walk);
                    std::string text;
                    for (auto it = path.rbegin(); it != path.rend(); ++it) {
                        if (!text.empty()) text += " -> ";
                        text += nameOf(*it);
                    }
                    return text;
                }
                next.push_back(dependent);
            }
        }
        frontier.swap(next);
    }
    return {};
}

std::vector<ObjectId> PartDocument::parametersReferencingParameter(ObjectId parameterId) const {
    std::vector<ObjectId> result;
    const Parameter* target = parameters_.findById(parameterId);
    if (target == nullptr) return result;
    for (const std::unique_ptr<Parameter>& candidate : parameters_.items()) {
        if (candidate->id() == parameterId) continue;
        if (candidate->expression().empty()) continue;
        for (const std::string& name : expressionVariableNames(candidate->expression())) {
            if (name != target->name()) continue;
            result.push_back(candidate->id());
            break;
        }
    }
    return result;
}

PartDocument::ExpressionWiringResult PartDocument::validateParameterExpressions() const {
    for (const std::unique_ptr<Parameter>& parameter : parameters_.items()) {
        const std::string& text = parameter->expression();
        if (text.empty()) continue;
        const std::string who = "parameter '" + parameter->name() + "'";

        if (!ExpressionDimensionOf(parameter->unit()).has_value())
            return {false, parameter->id(),
                    who + " carries an expression, but its unit takes a literal value only"};

        ExpressionParseResult parsed = ParseExpression(text);
        if (!parsed)
            return {false, parameter->id(), who + ": " + DescribeExpressionError(parsed.error)};

        for (const std::string& name : parsed.expression.referencedVariables()) {
            bool ambiguous = false;
            const Parameter* referenced = findParameterByExpressionName(name, ambiguous);
            if (ambiguous)
                return {false, parameter->id(),
                        who + " reads '#" + name + "', a name two parameters share"};
            if (referenced == nullptr)
                return {false, parameter->id(),
                        who + " reads '#" + name + "', which no parameter has"};
            if (referenced->id() == parameter->id())
                return {false, parameter->id(), who + " reads itself"};
            if (!ExpressionDimensionOf(referenced->unit()).has_value())
                return {false, parameter->id(),
                        who + " reads '#" + name + "', whose unit has no expression dimension"};
        }
    }
    return {};
}

PartDocument::ExpressionWiringResult PartDocument::rewireParameterExpressions() {
    if (ExpressionWiringResult invalid = validateParameterExpressions(); !invalid.ok)
        return invalid;

    for (const std::unique_ptr<Parameter>& parameter : parameters_.items()) {
        const std::string& text = parameter->expression();
        if (text.empty()) continue;
        for (ObjectId prerequisite : expressionPrerequisites(text)) {
            if (graph_.addDependency(parameter->id(), prerequisite)) continue;
            std::string path = describeDependencyPath(parameter->id(), prerequisite);
            if (path.empty()) path = parameter->name();
            return {false, parameter->id(),
                    "parameter '" + parameter->name() + "' takes part in a dependency cycle: " +
                        path + " -> " + parameter->name()};
        }
    }
    return {};
}

Body& PartDocument::addBody(std::string name) {
    auto item = std::make_unique<Body>(std::move(name));
    auto& ref = *item;
    bodies_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    return ref;
}

Body* PartDocument::findBodyNamed(const std::string& name) noexcept {
    for (const std::unique_ptr<Body>& body : bodies_)
        if (body->name() == name) return body.get();
    return nullptr;
}

Body& PartDocument::restoreBody(ObjectId id, std::string name) {
    // BEFORE construction: the Body constructor advances the id generator
    // (RestoreObjectId), so building-then-popping left that side effect behind
    // on every refusal. The registerObject check below stays as the backstop.
    requireUnusedId(id, "restoreBody");
    auto item = std::make_unique<Body>(id, std::move(name));
    auto& ref = *item;
    bodies_.push_back(std::move(item));
    if (!registry_.registerObject(ref.id(), &ref)) {
        bodies_.pop_back();
        throw std::runtime_error("restoreBody: id " + std::to_string(id) +
                                 " is already registered in this document");
    }
    return ref;
}


bool PartDocument::setSketchSupportFrame(ObjectId sketchId, ObjectId frameId) {
    Sketch* sketch = findSketchForEdit(sketchId);
    if (sketch == nullptr) return false;
    if (frameId != kInvalidObjectId && findFrame(frameId) == nullptr) return false;

    const ObjectId before = sketch->supportFrameId();
    if (before == frameId) return true;
    if (before != kInvalidObjectId) graph_.removeDependency(sketchId, before);
    sketch->setSupportFrameId(frameId);
    // The EDGE is the whole point (ADR-M10-003): with it, moving the frame
    // dirties the sketch, which dirties the pad, through the same M2 machinery
    // a Parameter edit uses. Without it the sketch would keep its old plane
    // until something else happened to dirty it -- stale geometry presented as
    // current, which is the defect class this project has fixed three times.
    if (frameId != kInvalidObjectId) graph_.addDependency(sketchId, frameId);
    graph_.markDirty(sketchId);
    syncFeatureStatesFromGraph();

    SketchSupportEdit edit;
    edit.sketchId = sketchId;
    edit.before = before;
    edit.after = frameId;
    recordDelta(edit, "Place " + sketch->name());
    return true;
}

bool PartDocument::restoreSketchSupportFrame(ObjectId sketchId, ObjectId frameId) {
    const bool wasApplying = applyingHistory_;
    applyingHistory_ = true;
    const bool ok = setSketchSupportFrame(sketchId, frameId);
    applyingHistory_ = wasApplying;
    return ok;
}

bool PartDocument::sketchSupportFrameIsMissing(ObjectId sketchId) const noexcept {
    const Sketch* sketch = findSketch(sketchId);
    if (sketch == nullptr) return false;
    const ObjectId frameId = sketch->supportFrameId();
    return frameId != kInvalidObjectId && findFrame(frameId) == nullptr;
}


SketchFrame PartDocument::effectiveSketchFrame(ObjectId sketchId) const noexcept {
    const Sketch* sketch = findSketch(sketchId);
    if (sketch == nullptr) return SketchFrame::WorldXY();
    const ObjectId frameId = sketch->supportFrameId();
    // No support frame: the sketch's own embedded plane, unchanged. Every
    // pre-M10 document and every world-XY sketch takes this path, so world-XY
    // stays a CASE of the general rule rather than a shortcut around it.
    if (frameId == kInvalidObjectId) return sketch->frame();
    return SketchFrame{worldTransform(frameId)};
}


void PartDocument::detachCurrentMaterial() noexcept {
    if (!material_) return;
    const ObjectId previous = material_->id();
    // The mass-properties node holds the material id BY VALUE, so a source
    // pointing at a material about to be destroyed has to be dropped too.
    if (massPropertiesNode_.materialId() == previous)
        massPropertiesNode_.setSource(massPropertiesNode_.boxFeatureId(), kInvalidObjectId);
    graph_.removeNode(previous);
    registry_.unregisterObject(previous);
}

Material& PartDocument::addMaterial(std::string name, double densityKgPerM3) {
    auto item = std::make_shared<Material>(std::move(name), densityKgPerM3);
    Material& ref = *item;
    // Unhook the outgoing material BEFORE the assignment destroys it. Doing it
    // afterwards would unregister the id the NEW material has just been given
    // in the id-reuse case, and leaving it undone is a read-after-free.
    detachCurrentMaterial();
    material_ = std::move(item);
    registry_.registerObject(ref.id(), &ref);
    graph_.addNode(ref.id());
    // Features created BEFORE this material exists would otherwise stay
    // unassigned and the part would weigh nothing.
    assignMaterialToFeatures();
    return ref;
}

Material& PartDocument::restoreMaterial(ObjectId id, std::string name, double densityKgPerM3,
                                        double elasticModulusPa, double poissonRatio,
                                        double yieldStrengthPa, ContactProperties contact) {
    // Checked BEFORE material_ is replaced. Assigning first and throwing after
    // DESTROYED the previous Material (its shared_ptr use_count was 1) while
    // the registry still held its address -- and the next recompute read the
    // density out of freed memory. The check existed; the ordering made it a
    // read-after-free instead of a rejection.
    requireUnusedId(id, "restoreMaterial");

    auto item = std::make_shared<Material>(id, std::move(name), densityKgPerM3, elasticModulusPa,
                                           poissonRatio, yieldStrengthPa, contact);
    Material& ref = *item;
    if (!registry_.registerObject(ref.id(), &ref))
        throw std::runtime_error("restoreMaterial: id " + std::to_string(id) +
                                 " could not be registered");
    // Registration succeeded, so the replacement is going ahead: unhook the
    // outgoing material before the assignment destroys it. The round-3 fix
    // moved the CHECK above the mutation, which made the throw path safe and
    // left the success path -- one line down -- still dangling.
    detachCurrentMaterial();
    material_ = std::move(item);
    graph_.addNode(ref.id()); // starts Dirty; graph states are not persisted
    return ref;
}

ObjectId PartDocument::massPropertiesNodeId() const noexcept {
    return massPropertiesNode_.id();
}

bool PartDocument::assignMaterialToFeatures() {
    if (!material_) return false;
    const ObjectId materialId = material_->id();
    bool assigned = false;
    for (const std::unique_ptr<Body>& body : bodies_) {
        for (const std::unique_ptr<Feature>& feature : body->features()) {
            auto* referencing = dynamic_cast<IMaterialReferencing*>(feature.get());
            if (referencing == nullptr) continue;
            if (referencing->materialId() == materialId) continue;
            referencing->setMaterialReference(materialId);
            assigned = true;
        }
    }
    // Rewire the mass-properties source too: the node reads density through
    // its own material edge, so re-pointing features alone would leave mass
    // computed from no material at all.
    if (massPropertiesNode_.boxFeatureId() != kInvalidObjectId) {
        rewireMassPropertiesSource(massPropertiesNode_.boxFeatureId(), materialId);
        assigned = true;
    }
    return assigned;
}

bool PartDocument::setMaterialDensity(double densityKgPerM3) {
    if (!material_) return false;
    material_->setDensity(densityKgPerM3);
    graph_.markDirty(material_->id()); // propagate to MassPropertiesNode
    syncFeatureStatesFromGraph();
    return true;
}

void PartDocument::setGeometryKernel(IGeometryKernel* kernel) noexcept {
    if (kernel_ == kernel) return;
    kernel_ = kernel;
    // Everything that builds geometry through the kernel is now stale by
    // definition -- including anything left Failed by the previous kernel's
    // absence, which the graph would otherwise never invoke again.
    for (const std::unique_ptr<Body>& body : bodies_)
        for (const std::unique_ptr<Feature>& feature : body->features())
            graph_.markDirty(feature->id());
    syncFeatureStatesFromGraph();
}

void PartDocument::setSketchSolver(ISketchSolver* solver) noexcept {
    if (sketchSolver_ == solver) return;
    sketchSolver_ = solver;
    for (const std::unique_ptr<Sketch>& sketch : sketches_) graph_.markDirty(sketch->id());
    syncFeatureStatesFromGraph();
}

void PartDocument::wireBoxFeature(BoxFeature& feature, ObjectId widthParameterId,
                                  ObjectId heightParameterId, ObjectId depthParameterId,
                                  ObjectId materialId) {
    addRecomputableNode(feature); // registry + graph node (IRecomputable*)
    addDependency(feature.id(), widthParameterId);
    addDependency(feature.id(), heightParameterId);
    addDependency(feature.id(), depthParameterId);

    rewireMassPropertiesSource(feature.id(), materialId);
}

// Extracted from wireBoxFeature in M4 so the Box and Pad paths share one
// implementation. Two copies of this wiring would be two places to get the
// detach-before-attach order wrong.
void PartDocument::rewireMassPropertiesSource(ObjectId solidFeatureId, ObjectId materialId) {
    // MassPropertiesNode joins the graph on first use (see the constructors'
    // comment): a document that never adds a solid feature must not carry a
    // permanently Dirty, permanently failing, edge-less recompute node.
    // Registry AND graph, together. removeObject(massPropertiesNodeId)
    // unregisters the node, and re-adding only the graph node left a node the
    // engine could schedule but not resolve -- "missing registry object" on
    // every recompute, permanently, in violation of the invariant
    // ObjectRegistry's own header states.
    if (!registry_.contains(massPropertiesNode_.id()))
        registry_.registerObject(massPropertiesNode_.id(), &massPropertiesNode_);
    if (!graph_.hasNode(massPropertiesNode_.id())) graph_.addNode(massPropertiesNode_.id());

    // Detach any previous source's edges FIRST so the graph never accumulates
    // stale prerequisites from an earlier feature or material (M3 scoping note,
    // ADR-M3-005).
    if (massPropertiesNode_.boxFeatureId() != kInvalidObjectId)
        removeDependency(massPropertiesNode_.id(), massPropertiesNode_.boxFeatureId());
    if (massPropertiesNode_.materialId() != kInvalidObjectId)
        removeDependency(massPropertiesNode_.id(), massPropertiesNode_.materialId());

    massPropertiesNode_.setSource(solidFeatureId, materialId);
    addDependency(massPropertiesNode_.id(), solidFeatureId); // solid -> MassProperties
    if (materialId != kInvalidObjectId)
        addDependency(massPropertiesNode_.id(), materialId); // Material -> MassProperties

    // The freshly added graph node starts Dirty, so this demotes a restored
    // feature that persisted a Valid ComputeState but, by design, restored no
    // runtime shape with it.
    syncFeatureStatesFromGraph();
}

// --- Sketch (M4) -----------------------------------------------------------

Sketch& PartDocument::addSketch(std::string name, SketchFrame frame) {
    auto item = std::make_unique<Sketch>(std::move(name));
    item->setFrame(frame);
    Sketch& ref = *item;
    sketches_.push_back(std::move(item));
    // Checked, not ignored: registerObject returns false on a duplicate id, and
    // a silently unregistered sketch is invisible to both the recompute engine
    // and PadFeature's profile lookup (see restoreSketch).
    // Checked in EVERY configuration, not only asserted in Debug. An assert
    // disappears in Release, and a sketch that failed to register is in
    // sketches_ and in the graph but invisible to the recompute engine --
    // and removeObject would then return false and leave it behind.
    // restoreSketch was hardened after M5; addSketch was the one path left.
    if (!registry_.registerObject(ref.id(), &ref)) {
        sketches_.pop_back();
        throw std::runtime_error("addSketch: id " + std::to_string(ref.id()) +
                                 " is already registered in this document");
    }
    // Since M5 a Sketch is a RECOMPUTABLE node, not a bare dirty source: its
    // geometry is derived from its constraints. The engine reaches it through
    // ObjectRegistry::findRecomputable, which upcasts from the Sketch*
    // alternative -- so this stays registerObject rather than
    // addRecomputableNode, and PadFeature can still resolve the same handle as
    // a Sketch* to read its profile.
    graph_.addNode(ref.id());
    return ref;
}

Sketch& PartDocument::restoreSketch(ObjectId id, std::string name, SketchFrame frame) {
    requireUnusedId(id, "restoreSketch"); // same reason as restoreBody
    auto item = std::make_unique<Sketch>(id, std::move(name), frame);
    Sketch& ref = *item;
    sketches_.push_back(std::move(item));
    // Registered as Sketch*, NOT via addRecomputableNode: the registry answers
    // "is this recomputable?" from the static type, so one handle serves both
    // the recompute engine and PadFeature's profile lookup.
    //
    // The return value is CHECKED. Ignoring it is how a reloaded document ended
    // up with a sketch that was never registered -- resolvable only as the
    // MassPropertiesNode it collided with -- while every save/load test passed,
    // because they all ran in one process where no collision was possible.
    if (!registry_.registerObject(ref.id(), &ref)) {
        sketches_.pop_back();
        throw std::runtime_error("restoreSketch: id " + std::to_string(id) +
                                 " is already registered in this document");
    }
    graph_.addNode(ref.id()); // starts Dirty; graph states are not persisted
    return ref;
}

std::vector<const Sketch*> PartDocument::sketches() const {
    std::vector<const Sketch*> result;
    result.reserve(sketches_.size());
    for (const std::unique_ptr<Sketch>& sketch : sketches_) result.push_back(sketch.get());
    return result;
}

const Sketch* PartDocument::findSketch(ObjectId id) const noexcept {
    for (const std::unique_ptr<Sketch>& sketch : sketches_)
        if (sketch->id() == id) return sketch.get();
    return nullptr;
}

Sketch* PartDocument::findSketchForEdit(ObjectId id) noexcept {
    for (const std::unique_ptr<Sketch>& sketch : sketches_)
        if (sketch->id() == id) return sketch.get();
    return nullptr;
}

void PartDocument::reconcileSketchParameterEdges(ObjectId sketchId) {
    const Sketch* sketch = findSketch(sketchId);
    if (sketch == nullptr) return;

    std::vector<ObjectId> wanted;
    for (const SketchConstraint& constraint : sketch->constraints()) {
        const ObjectId parameterId = BoundParameterId(constraint.data);
        if (parameterId == kInvalidObjectId) continue;
        // Only PARAMETER ids, because only Parameter prerequisites are removed
        // below. Adding an edge for a bound id that is not a Parameter -- a
        // Material id, say, reachable through editSketch -- wired an edge this
        // function could never take away again: after the offending constraint
        // was deleted the edge survived, and a density edit kept re-solving a
        // sketch that read nothing from it. That is verbatim the phantom-edge
        // defect this reconciler was written to eliminate, re-created by it.
        // Add and remove must range over the same set or they cannot agree.
        if (parameters_.findById(parameterId) == nullptr) continue;
        if (std::find(wanted.begin(), wanted.end(), parameterId) == wanted.end())
            wanted.push_back(parameterId);
    }

    // Drop edges from Parameters this sketch no longer binds. Only PARAMETER
    // prerequisites are touched: a sketch has no other kind today, but saying
    // so explicitly keeps this from quietly eating a future edge kind.
    for (ObjectId prerequisite : graph_.prerequisitesOf(sketchId)) {
        if (parameters_.findById(prerequisite) == nullptr) continue;
        if (std::find(wanted.begin(), wanted.end(), prerequisite) != wanted.end()) continue;
        removeDependency(sketchId, prerequisite);
    }
    for (ObjectId parameterId : wanted) addDependency(sketchId, parameterId);
}

void PartDocument::reconcileAllSketchParameterEdges() {
    for (const std::unique_ptr<Sketch>& sketch : sketches_)
        reconcileSketchParameterEdges(sketch->id());
}

bool PartDocument::editSketch(ObjectId sketchId, const std::function<void(Sketch&)>& edit) {
    Sketch* sketch = findSketchForEdit(sketchId);
    if (sketch == nullptr || !edit) return false;
    edit(*sketch);
    // The callback may have added, removed or cascaded constraints through
    // Sketch's own API, so the graph is reconciled here rather than trusted.
    reconcileSketchParameterEdges(sketchId);
    // Dirtying is not optional and not the caller's responsibility: an edited
    // sketch whose dependents were never marked stale would keep reporting a
    // solid built from geometry that no longer exists.
    graph_.markDirty(sketchId);
    syncFeatureStatesFromGraph();
    return true;
}

SketchEntityId PartDocument::addSketchEntity(ObjectId sketchId, SketchGeometry geometry,
                                            bool construction) {
    Sketch* sketch = findSketchForEdit(sketchId);
    if (sketch == nullptr) return kInvalidSketchEntityId;

    // Dispatched to the typed add* methods rather than to a generic one,
    // because THEY are what run IsValidSketchGeometry -- a degenerate line must
    // still be refused when it arrives through the facade, or the facade
    // becomes the hole the validator does not cover.
    SketchEntityId id = kInvalidSketchEntityId;
    if (const auto* point = std::get_if<SketchPoint>(&geometry)) {
        id = sketch->addPoint(point->position);
    } else if (const auto* line = std::get_if<SketchLine>(&geometry)) {
        id = sketch->addLine(line->start, line->end);
    } else if (const auto* circle = std::get_if<SketchCircle>(&geometry)) {
        id = sketch->addCircle(circle->center, circle->radiusMm);
    } else if (const auto* arc = std::get_if<SketchArc>(&geometry)) {
        id = sketch->addArc(arc->center, arc->radiusMm, arc->startAngleRad, arc->endAngleRad,
                            arc->counterClockwise);
    } else if (const auto* full = std::get_if<SketchEllipse>(&geometry)) {
        id = sketch->addEllipse(full->center, full->majorRadiusMm, full->minorRadiusMm,
                                full->rotationRad);
    } else if (const auto* piece = std::get_if<SketchEllipticalArc>(&geometry)) {
        id = sketch->addEllipticalArc(piece->center, piece->majorRadiusMm, piece->minorRadiusMm,
                                      piece->rotationRad, piece->startParamRad,
                                      piece->endParamRad, piece->counterClockwise);
    } else if (const auto* spline = std::get_if<SketchSpline>(&geometry)) {
        id = sketch->addSpline(spline->points, spline->closed);
    }
    if (id == kInvalidSketchEntityId) return id;

    // Recorded from what the SKETCH stored, not from the argument: add* may
    // normalise, and an undo that restores the argument would restore geometry
    // the sketch never held.
    SketchEntityExistenceEdit edit;
    edit.sketchId = sketchId;
    edit.entityId = id;
    const SketchEntity* stored = sketch->findEntity(id);
    edit.geometry = stored != nullptr ? stored->geometry : geometry;
    // IN THE SAME STEP as the entity, not by calling the construction facade
    // afterwards (M17.17). That facade opens a ScopedTransaction, and doing so
    // from inside a caller's open transaction COMMITS the caller's -- the
    // polygon's own commit then found nothing open, reported failure, and rolled
    // back every line it had just made.
    if (construction) sketch->setEntityConstruction(id, true);
    edit.construction = construction;
    edit.addedByTheEdit = true;
    recordDelta(edit, "Add sketch geometry");

    graph_.markDirty(sketchId);
    syncFeatureStatesFromGraph();
    return id;
}

SketchConstraintId PartDocument::addSketchConstraint(ObjectId sketchId,
                                                    SketchConstraintData data) {
    Sketch* sketch = findSketchForEdit(sketchId);
    if (sketch == nullptr) return kInvalidSketchConstraintId;

    // The bound id must actually BE a Parameter. Without this the facade
    // accepted a Length bound to a Body id, or to nothing at all: the graph
    // edge silently failed to wire (no such node), the document looked fine,
    // and then every save failed forever with a validation error the user had
    // no way to connect to what they did. The save-side validator was the only
    // thing standing between that and a file the loader would reject.
    // The bound id must exist AND be a Parameter.
    //
    // Rejecting only the "not a Parameter" half left the "bound to nothing"
    // half open, and that half was worse: the solve problem was built with
    // target = 0.0 and no complaint -- asking the solver to drive a line to
    // zero length -- while validateSaveable skipped its check for an invalid
    // id, so the document SAVED CLEANLY and the loader then refused it forever
    // ("missing required field 'parameterId'"). A file that saves and can
    // never be loaded back is exactly what ADR-M3-008 exists to prevent, and
    // it was reachable through the facade hardened for this very finding.
    const ObjectId parameterId = BoundParameterId(data);
    if (IsDimensional(data)) {
        if (parameterId == kInvalidObjectId) return kInvalidSketchConstraintId;
        if (parameters_.findById(parameterId) == nullptr) return kInvalidSketchConstraintId;
    }

    const SketchConstraintId id = sketch->addConstraint(std::move(data));
    if (id == kInvalidSketchConstraintId) return id;

    // Parameter -> Sketch. This edge is the whole reason the facade exists: it
    // is what makes "edit Width, the sketch re-solves" fall out of M2's
    // propagation instead of needing a mechanism of its own.
    reconcileSketchParameterEdges(sketchId);

    // M12: recorded AFTER the edge is wired, and from what the sketch stored,
    // so an undo/redo pair reproduces the same constraint AND the same edge.
    SketchConstraintExistenceEdit undoEdit;
    undoEdit.sketchId = sketchId;
    undoEdit.constraintId = id;
    const SketchConstraint* stored = sketch->findConstraint(id);
    if (stored != nullptr) undoEdit.data = stored->data;
    undoEdit.addedByTheEdit = true;
    recordDelta(undoEdit, "Add constraint");

    graph_.markDirty(sketchId);
    syncFeatureStatesFromGraph();
    return id;
}

bool PartDocument::removeSketchConstraint(ObjectId sketchId,
                                          SketchConstraintId constraintId) {
    Sketch* sketch = findSketchForEdit(sketchId);
    if (sketch == nullptr) return false;
    const SketchConstraint* constraint = sketch->findConstraint(constraintId);
    if (constraint == nullptr) return false;

    const ObjectId parameterId = BoundParameterId(constraint->data);
    // Copied BEFORE the removal: `constraint` dangles the moment the sketch
    // erases it, and the undo record needs the definition, not the pointer.
    const SketchConstraintData removedData = constraint->data;
    const std::pair<bool, Vec2> placementBefore = CurrentPlacement(*sketch, constraintId);
    const Sketch::DimensionFormat* formatPtr = sketch->dimensionFormat(constraintId);
    const std::pair<bool, Sketch::DimensionFormat> formatBefore =
        formatPtr != nullptr ? std::make_pair(true, *formatPtr)
                             : std::make_pair(false, Sketch::DimensionFormat{});
    // ONE undo step for the constraint AND the presentation that went with it.
    ScopedTransaction transaction(*this, "Delete constraint");
    if (!sketch->removeConstraint(constraintId)) return false;

    // The FORMAT goes with it too, recorded before the constraint for the same
    // reason the placement is.
    if (formatBefore.first) {
        SketchDimensionFormatEdit formatEdit;
        formatEdit.sketchId = sketchId;
        formatEdit.constraintId = constraintId;
        formatEdit.beforePrefix = formatBefore.second.prefix;
        formatEdit.beforeSuffix = formatBefore.second.suffix;
        formatEdit.beforePlus = formatBefore.second.plusTolerance;
        formatEdit.beforeMinus = formatBefore.second.minusTolerance;
        recordDelta(formatEdit, "Delete constraint");
    }

    // The placement goes with it, and has to come BACK with it. Recorded
    // first, so the reverse replay restores the constraint before the
    // placement that refers to it.
    if (placementBefore.first) {
        SketchDimensionPlacementEdit placementEdit;
        placementEdit.sketchId = sketchId;
        placementEdit.constraintId = constraintId;
        placementEdit.hasBefore = true;
        placementEdit.before = placementBefore.second;
        placementEdit.hasAfter = false;
        recordDelta(placementEdit, "Delete constraint");
    }

    SketchConstraintExistenceEdit undoEdit;
    undoEdit.sketchId = sketchId;
    undoEdit.constraintId = constraintId;
    undoEdit.data = removedData;
    undoEdit.addedByTheEdit = false;
    recordDelta(undoEdit, "Delete constraint");

    // One reconciler, not a per-path rule: it drops the edge only when nothing
    // else on this sketch still binds that Parameter, because two dimensions
    // legitimately share one.
    (void)parameterId;
    reconcileSketchParameterEdges(sketchId);

    graph_.markDirty(sketchId);
    syncFeatureStatesFromGraph();
    return transaction.commit();
}

bool PartDocument::removeSketchEntity(ObjectId sketchId, SketchEntityId entityId) {
    Sketch* sketch = findSketchForEdit(sketchId);
    if (sketch == nullptr) return false;

    // Everything the removal is about to destroy, captured while it still
    // exists. `EntityRemoval` reports which constraints went but not what they
    // WERE, and an undo has to put the definitions back, not the ids.
    const SketchEntity* doomed = sketch->findEntity(entityId);
    if (doomed == nullptr) return false;
    const SketchGeometry removedGeometry = doomed->geometry;
    std::vector<SketchConstraintExistenceEdit> cascadedUndo;
    std::vector<SketchDimensionPlacementEdit> cascadedPlacements;
    for (const SketchConstraintId constraintId : sketch->constraintsReferencing(entityId)) {
        const SketchConstraint* constraint = sketch->findConstraint(constraintId);
        if (constraint == nullptr) continue;
        SketchConstraintExistenceEdit undoEdit;
        undoEdit.sketchId = sketchId;
        undoEdit.constraintId = constraintId;
        undoEdit.data = constraint->data;
        undoEdit.addedByTheEdit = false;
        cascadedUndo.push_back(std::move(undoEdit));
        // ...and whatever placement went with it, so a deleted-then-undone
        // dimension comes back where the user had put it rather than snapping
        // to automatic.
        const std::pair<bool, Vec2> placement = CurrentPlacement(*sketch, constraintId);
        if (placement.first) {
            SketchDimensionPlacementEdit placementEdit;
            placementEdit.sketchId = sketchId;
            placementEdit.constraintId = constraintId;
            placementEdit.hasBefore = true;
            placementEdit.before = placement.second;
            placementEdit.hasAfter = false;
            cascadedPlacements.push_back(placementEdit);
        }
    }

    // ONE undo step for the entity, every constraint that cascaded with it,
    // and every dimension placement those constraints carried.
    ScopedTransaction transaction(*this, "Delete sketch geometry");
    const Sketch::EntityRemoval removal = sketch->removeEntityCascading(entityId);
    if (!removal.removed) return false;

    // ORDER IS LOAD-BEARING. Deltas undo in reverse, so the entity delta must
    // be pushed LAST to be undone FIRST: a constraint cannot be restored onto
    // geometry that is not back yet.
    // Placements FIRST: reverse replay then restores the entity, then its
    // constraints, then where their values sat.
    for (SketchDimensionPlacementEdit& placementEdit : cascadedPlacements)
        recordDelta(placementEdit, "Delete sketch geometry");
    for (SketchConstraintExistenceEdit& undoEdit : cascadedUndo)
        recordDelta(std::move(undoEdit), "Delete sketch geometry");

    SketchEntityExistenceEdit entityUndo;
    entityUndo.sketchId = sketchId;
    entityUndo.entityId = entityId;
    entityUndo.geometry = removedGeometry;
    entityUndo.addedByTheEdit = false;
    recordDelta(entityUndo, "Delete sketch geometry");

    // The released list is not a removal list -- a surviving constraint may
    // still bind the same Parameter -- so the edges are reconciled against the
    // constraint set that actually remains.
    (void)removal;
    reconcileSketchParameterEdges(sketchId);

    graph_.markDirty(sketchId);
    syncFeatureStatesFromGraph();
    return transaction.commit();
}

SketchSolveStatus PartDocument::previewSketchDrag(ObjectId sketchId,
                                                  const SketchElementRef& point, Vec2 targetMm) {
    if (!std::isfinite(targetMm.x) || !std::isfinite(targetMm.y))
        return SketchSolveStatus::InvalidInput;
    Sketch* sketch = findSketchForEdit(sketchId);
    if (sketch == nullptr) return SketchSolveStatus::InvalidInput;
    const SketchEntity* entity = sketch->findEntity(point.entityId);
    if (entity == nullptr) return SketchSolveStatus::InvalidInput;

    // BUILT BEFORE ANYTHING MOVES, and this ordering is load-bearing.
    //
    // A FixConstraint's target is read out of the geometry at build time -- it
    // means "where this point IS". Writing the cursor into the sketch first and
    // building afterwards therefore re-baselines every Fix in the sketch onto
    // the cursor, and a pinned point follows the mouse anywhere while still
    // reporting Solved. That shipped for about ten minutes and a test caught
    // it; the ordering is the fix, so do not move this line.
    //
    // The cursor is applied as an INITIAL GUESS instead, and the sketch's own
    // geometry is left untouched until a solve has converged.
    if (sketch->constraints().empty()) {
        // Nothing to pull it back: the seed IS the answer, and there is no
        // problem to build.
        SketchGeometry seeded = entity->geometry;
        if (!SeedDraggedPoint(seeded, point.subElement, targetMm))
            return SketchSolveStatus::InvalidInput;
        if (!sketch->setEntityGeometry(point.entityId, seeded))
            return SketchSolveStatus::InvalidInput;
        return SketchSolveStatus::UnderConstrained;
    }
    if (sketchSolver_ == nullptr) return SketchSolveStatus::InvalidInput;

    const BuildProblemResult built = BuildSolveProblem(*sketch, registry_);
    if (!built) return SketchSolveStatus::InvalidInput;

    int pinU = -1;
    int pinV = -1;
    for (std::size_t i = 0; i < built.problem.variables.size(); ++i) {
        const SolveVariable& variable = built.problem.variables[i];
        if (variable.entityId != point.entityId) continue;
        if (variable.subElement != point.subElement) continue;
        if (variable.component == SolveVariable::Component::U) pinU = static_cast<int>(i);
        if (variable.component == SolveVariable::Component::V) pinV = static_cast<int>(i);
    }
    // No variables means the reference names nothing the solver can move -- an
    // arc's tip, say (ADR-M12-003). Refused rather than dragging the nearest
    // thing that does have variables, which is not what was grabbed.
    if (pinU < 0 || pinV < 0) return SketchSolveStatus::InvalidInput;

    // The cursor as the STARTING POINT. Gauss-Newton converges to a solution
    // near where it starts, which is what makes both attempts below land
    // somewhere the user recognises.
    SketchSolveProblem seeded = built.problem;
    seeded.initialValues[static_cast<std::size_t>(pinU)] = targetMm.x;
    seeded.initialValues[static_cast<std::size_t>(pinV)] = targetMm.y;

    // TWO ATTEMPTS, and both are needed -- each alone gets a whole class of
    // drags wrong, and finding that out cost two failing tests apiece:
    //
    //  1. PINNED: the dragged point is HELD at the cursor by a pair of hard
    //     residuals. This is what makes dragging a corner work -- the coincident
    //     neighbour is pulled exactly to the cursor. Seeding alone lets the
    //     solver satisfy the coincidence by moving BOTH points, and
    //     minimum-norm does precisely that: the corner meets the cursor
    //     halfway and the drag feels broken.
    //
    //  2. SEEDED ONLY: a pin that contradicts the model IS a contradiction, and
    //     the solver rightly calls it Conflicting -- so a length-dimensioned
    //     line dragged past its own length never moved at all. Dropping the pin
    //     asks the weaker, correct question: "start near here and be legal",
    //     which slides, swings, and snaps back exactly as expected.
    //
    // Pinned first, because it is the stronger promise: honour the cursor when
    // the model allows it, get as close as the constraints permit when it does
    // not.
    SketchSolveProblem pinned = seeded;
    SolveResidual pin;
    pin.kind = SolveResidual::Kind::FixedU;
    pin.vars[0] = pinU;
    pin.target = targetMm.x;
    pinned.residuals.push_back(pin);
    pin.kind = SolveResidual::Kind::FixedV;
    pin.vars[0] = pinV;
    pin.target = targetMm.y;
    pinned.residuals.push_back(pin);

    const SketchSolveResult exact = sketchSolver_->solve(pinned);
    if (exact && CommitSolvedGeometry(*sketch, pinned, exact)) return exact.status;

    const SketchSolveResult relaxed = sketchSolver_->solve(seeded);
    // NOTHING has been written to the sketch yet on this path, so a refusal
    // needs no rollback -- the geometry is still exactly what the user last saw.
    if (!relaxed) return relaxed.status;
    if (!CommitSolvedGeometry(*sketch, seeded, relaxed))
        return SketchSolveStatus::NumericalFailure;
    return relaxed.status;
}

std::vector<std::pair<SketchEntityId, SketchGeometry>> PartDocument::sketchGeometrySnapshot(
    ObjectId sketchId) const {
    std::vector<std::pair<SketchEntityId, SketchGeometry>> snapshot;
    const Sketch* sketch = findSketch(sketchId);
    if (sketch == nullptr) return snapshot;
    snapshot.reserve(sketch->entities().size());
    for (const SketchEntity& entity : sketch->entities())
        snapshot.push_back({entity.id, entity.geometry});
    return snapshot;
}

std::size_t PartDocument::commitSketchDrag(
    ObjectId sketchId, const std::vector<std::pair<SketchEntityId, SketchGeometry>>& before,
    const std::string& label) {
    Sketch* sketch = findSketchForEdit(sketchId);
    if (sketch == nullptr) return 0;

    ScopedTransaction transaction(*this, label);
    std::size_t moved = 0;
    for (const auto& entry : before) {
        const SketchEntity* current = sketch->findEntity(entry.first);
        if (current == nullptr) continue;
        if (SameSketchGeometryValue(current->geometry, entry.second)) continue;
        SketchEntityGeometryEdit edit;
        edit.sketchId = sketchId;
        edit.entityId = entry.first;
        edit.before = entry.second;
        edit.after = current->geometry;
        recordDelta(edit, label);
        ++moved;
    }
    if (moved == 0) return 0;
    graph_.markDirty(sketchId);
    syncFeatureStatesFromGraph();
    if (!transaction.commit()) return 0;
    return moved;
}

bool PartDocument::restoreSketchGeometry(
    ObjectId sketchId, const std::vector<std::pair<SketchEntityId, SketchGeometry>>& before) {
    Sketch* sketch = findSketchForEdit(sketchId);
    if (sketch == nullptr) return false;
    // No delta and no dirty: this is an abandoned preview, and the document is
    // being put back exactly where it already believed it was.
    for (const auto& entry : before) (void)sketch->setEntityGeometry(entry.first, entry.second);
    return true;
}

bool PartDocument::setSketchEntityGeometry(ObjectId sketchId, SketchEntityId entityId,
                                           SketchGeometry geometry) {
    Sketch* sketch = findSketchForEdit(sketchId);
    if (sketch == nullptr) return false;
    const SketchEntity* existing = sketch->findEntity(entityId);
    if (existing == nullptr) return false;

    SketchEntityGeometryEdit edit;
    edit.sketchId = sketchId;
    edit.entityId = entityId;
    edit.before = existing->geometry;
    edit.after = geometry;

    ScopedTransaction transaction(*this, "Reshape sketch geometry");
    if (!sketch->setEntityGeometry(entityId, std::move(geometry))) return false;
    recordDelta(edit, "Reshape sketch geometry");
    graph_.markDirty(sketchId);
    syncFeatureStatesFromGraph();
    return transaction.commit();
}

std::size_t PartDocument::setSketchEntitiesConstruction(
    ObjectId sketchId, const std::vector<SketchEntityId>& entityIds, bool construction) {
    Sketch* sketch = findSketchForEdit(sketchId);
    if (sketch == nullptr) return 0;

    ScopedTransaction transaction(*this, construction ? "Make construction geometry"
                                                      : "Make normal geometry");
    std::size_t changed = 0;
    for (const SketchEntityId id : entityIds) {
        if (sketch->findEntity(id) == nullptr) continue;
        const bool before = sketch->isConstruction(id);
        if (before == construction) continue;
        if (!sketch->setEntityConstruction(id, construction)) continue;

        SketchEntityConstructionEdit edit;
        edit.sketchId = sketchId;
        edit.entityId = id;
        edit.before = before;
        edit.after = construction;
        recordDelta(edit, construction ? "Make construction geometry" : "Make normal geometry");
        ++changed;
    }
    // Nothing changed: the ScopedTransaction's destructor rolls back, so an
    // idle command leaves no empty step on the undo stack for the user to walk
    // back through.
    if (changed == 0) return 0;
    // DIRTIED, unlike a dimension placement: the flag decides whether an edge
    // reaches the profile, so a pad built on this sketch is a different solid
    // afterwards.
    graph_.markDirty(sketchId);
    syncFeatureStatesFromGraph();
    if (!transaction.commit()) return 0;
    return changed;
}

bool PartDocument::setSketchTrackedFace(ObjectId sketchId, FaceQuery query) {
    Sketch* sketch = findSketchForEdit(sketchId);
    if (sketch == nullptr) return false;
    if (!query.createdBy.has_value()) return false; // nothing to depend on

    const ObjectId owner = *query.createdBy;
    // The feature must exist AND produce a solid: a face query against
    // something with no shape can never be answered, and accepting it would
    // store a sketch that fails on every recompute from now on.
    bool ownerIsSolid = false;
    for (const std::unique_ptr<Body>& body : bodies_)
        for (const std::unique_ptr<Feature>& feature : body->features())
            if (feature->id() == owner &&
                dynamic_cast<const ISolidFeature*>(feature.get()) != nullptr)
                ownerIsSolid = true;
    if (!ownerIsSolid) return false;

    // The EDGE FIRST, because addDependency is what refuses a cycle -- a
    // sketch tracking a face of a feature built from that same sketch. Setting
    // the query first and discovering the cycle after would leave the sketch
    // holding a query the graph will not support.
    if (!addDependency(sketchId, owner)) return false;

    sketch->setTrackedFace(std::move(query));
    // Dirty, because the plane this sketch reports is now derived from
    // something else and has not been derived yet.
    graph_.markDirty(sketchId);
    syncFeatureStatesFromGraph();
    return true;
}

namespace {

// Is this name already taken by something the tree shows? Names are how a user
// picks what to delete or edit, and for a PARAMETER a duplicate is worse than
// confusing: expressions resolve by name and findByName answers with the first
// match (ADR-M17-038).
bool PartNameIsFree(const PartDocument& document, ObjectId self, const std::string& name) {
    for (const auto& parameter : document.parameters().items())
        if (parameter->id() != self && parameter->name() == name) return false;
    for (const Sketch* sketch : document.sketches())
        if (sketch->id() != self && sketch->name() == name) return false;
    for (const auto& body : document.bodies()) {
        if (body->id() != self && body->name() == name) return false;
        for (const auto& feature : body->features())
            if (feature->id() != self && feature->name() == name) return false;
    }
    if (document.material() != nullptr && document.material()->id() != self &&
        document.material()->name() == name)
        return false;
    return true;
}

} // namespace

bool PartDocument::ownNameIsTaken(const std::string& name, ObjectId except) const {
    return !PartNameIsFree(*this, except, name);
}

void PartDocument::applyOwnName(ObjectId id, const std::string& name) {
    for (const auto& parameter : parameters_.items())
        if (parameter->id() == id) {
            parameter->setName(name);
            return;
        }
    for (const std::unique_ptr<Sketch>& sketch : sketches_)
        if (sketch->id() == id) {
            sketch->setName(name);
            return;
        }
    for (const std::unique_ptr<Body>& body : bodies_) {
        if (body->id() == id) {
            body->setName(name);
            return;
        }
        for (const std::unique_ptr<Feature>& feature : body->features())
            if (feature->id() == id) {
                feature->setName(name);
                return;
            }
    }
    if (material_ && material_->id() == id) material_->setName(name);
}


std::string PartDocument::ownObjectName(ObjectId id) const {
    for (const auto& parameter : parameters_.items())
        if (parameter->id() == id) return parameter->name();
    for (const Sketch* sketch : sketches()) 
        if (sketch->id() == id) return sketch->name();
    for (const auto& body : bodies_) {
        if (body->id() == id) return body->name();
        for (const auto& feature : body->features())
            if (feature->id() == id) return feature->name();
    }
    if (material_ && material_->id() == id) return material_->name();
    return {};
}

bool PartDocument::setSketchConstraintDriven(ObjectId sketchId,
                                            SketchConstraintId constraintId, bool driven) {
    Sketch* sketch = findSketchForEdit(sketchId);
    if (sketch == nullptr) return false;
    if (sketch->isConstraintDriven(constraintId) == driven) return true; // nothing to record

    ScopedTransaction transaction(*this, driven ? "Make dimension a reference"
                                                : "Make dimension drive");
    if (!sketch->setConstraintDriven(constraintId, driven)) return false;

    SketchConstraintDrivenEdit edit;
    edit.sketchId = sketchId;
    edit.constraintId = constraintId;
    edit.before = !driven;
    edit.after = driven;
    recordDelta(edit, driven ? "Make dimension a reference" : "Make dimension drive");
    graph_.markDirty(sketchId);
    syncFeatureStatesFromGraph();
    // EXPLICIT. ScopedTransaction's destructor ABORTS what was not committed,
    // which is what makes a facade that bails out halfway safe -- and what
    // silently rolled this one back when the commit was left out.
    return transaction.commit();
}

bool PartDocument::setDrivenParameterValue(ObjectId parameterId, double value) {
    ObjectRegistry::ObjectRef* ref = registry_.find(parameterId);
    if (ref == nullptr) return false;
    auto* const* parameter = std::get_if<Parameter*>(ref);
    if (parameter == nullptr) return false;
    if ((*parameter)->value() == value) return true; // nothing to publish
    (*parameter)->setValue(value);
    // Dirtied so anything reading this number recomputes -- an expression
    // built on a reference dimension is exactly the case this is for. The
    // sketch that produced it is mid-recompute and is not a dependent of its
    // own driven parameter, so this cannot loop.
    graph_.markDirty(parameterId);
    syncFeatureStatesFromGraph();
    return true;
}

bool PartDocument::setFeatureEdgeSelection(ObjectId featureId, EdgeSelection selection) {
    for (const std::unique_ptr<Body>& body : bodies_)
        for (const std::unique_ptr<Feature>& feature : body->features()) {
            if (feature->id() != featureId) continue;
            auto* dress = dynamic_cast<EdgeDressFeature*>(feature.get());
            if (dress == nullptr) return false; // not a feature that dresses edges
            dress->setEdgeSelection(std::move(selection));
            // DIRTY, because the shape this feature produces has changed even
            // though no number did.
            graph_.markDirty(featureId);
            syncFeatureStatesFromGraph();
            return true;
        }
    return false;
}

std::size_t PartDocument::addSketchReferences(ObjectId sketchId,
                                             const std::vector<SketchGeometry>& geometry) {
    Sketch* sketch = findSketchForEdit(sketchId);
    if (sketch == nullptr) return 0;
    std::size_t added = 0;
    for (const SketchGeometry& item : geometry)
        if (sketch->addReference(item) != kInvalidSketchReferenceId) ++added;
    // NOT dirtied. A reference contributes no profile edge and no solver
    // variable, so nothing downstream of this sketch computes differently
    // because of it -- marking it dirty would force a rebuild of every feature
    // below, to produce byte-identical geometry.
    return added;
}

bool PartDocument::setSketchDimensionPlacement(ObjectId sketchId,
                                               SketchConstraintId constraintId, Vec2 labelMm) {
    Sketch* sketch = findSketchForEdit(sketchId);
    if (sketch == nullptr) return false;
    const std::pair<bool, Vec2> before = CurrentPlacement(*sketch, constraintId);
    if (!sketch->setDimensionPlacement(constraintId, labelMm)) return false;

    SketchDimensionPlacementEdit edit;
    edit.sketchId = sketchId;
    edit.constraintId = constraintId;
    edit.hasBefore = before.first;
    edit.before = before.second;
    edit.hasAfter = true;
    edit.after = labelMm;
    recordDelta(edit, "Move dimension");
    // NOT dirtied. Where a value SITS changes nothing the solver or any
    // feature reads, so marking the sketch dirty would make moving a label
    // recompute the whole downstream chain.
    return true;
}

bool PartDocument::previewSketchDimensionPlacement(ObjectId sketchId,
                                                  SketchConstraintId constraintId,
                                                  Vec2 labelMm) {
    Sketch* sketch = findSketchForEdit(sketchId);
    if (sketch == nullptr) return false;
    // No delta, no markDirty. See the header for why both are deliberate.
    return sketch->setDimensionPlacement(constraintId, labelMm);
}

bool PartDocument::previewClearSketchDimensionPlacement(ObjectId sketchId,
                                                        SketchConstraintId constraintId) {
    Sketch* sketch = findSketchForEdit(sketchId);
    if (sketch == nullptr) return false;
    return sketch->clearDimensionPlacement(constraintId);
}

bool PartDocument::setSketchDimensionFormat(ObjectId sketchId,
                                           SketchConstraintId constraintId,
                                           const Sketch::DimensionFormat& format) {
    Sketch* sketch = findSketchForEdit(sketchId);
    if (sketch == nullptr) return false;

    Sketch::DimensionFormat before;
    before.constraintId = constraintId;
    if (const Sketch::DimensionFormat* existing = sketch->dimensionFormat(constraintId))
        before = *existing;

    if (!sketch->setDimensionFormat(constraintId, format)) return false;

    SketchDimensionFormatEdit edit;
    edit.sketchId = sketchId;
    edit.constraintId = constraintId;
    edit.beforePrefix = before.prefix;
    edit.beforeSuffix = before.suffix;
    edit.beforePlus = before.plusTolerance;
    edit.beforeMinus = before.minusTolerance;
    edit.afterPrefix = format.prefix;
    edit.afterSuffix = format.suffix;
    edit.afterPlus = format.plusTolerance;
    edit.afterMinus = format.minusTolerance;
    recordDelta(edit, "Format dimension");
    // NOT dirtied: how a value READS changes nothing the solver computes.
    return true;
}

bool PartDocument::clearSketchDimensionPlacement(ObjectId sketchId,
                                                 SketchConstraintId constraintId) {
    Sketch* sketch = findSketchForEdit(sketchId);
    if (sketch == nullptr) return false;
    const std::pair<bool, Vec2> before = CurrentPlacement(*sketch, constraintId);
    if (!sketch->clearDimensionPlacement(constraintId)) return false;

    SketchDimensionPlacementEdit edit;
    edit.sketchId = sketchId;
    edit.constraintId = constraintId;
    edit.hasBefore = before.first;
    edit.before = before.second;
    edit.hasAfter = false;
    recordDelta(edit, "Auto-place dimension");
    return true;
}

std::vector<SketchConstraintId> PartDocument::constraintsBindingParameter(
    ObjectId parameterId) const {
    std::vector<SketchConstraintId> ids;
    if (parameterId == kInvalidObjectId) return ids;
    for (const std::unique_ptr<Sketch>& sketch : sketches_)
        for (const SketchConstraint& constraint : sketch->constraints())
            if (BoundParameterId(constraint.data) == parameterId) ids.push_back(constraint.id);
    return ids;
}

std::vector<ObjectId> PartDocument::featuresReferencingSketch(ObjectId sketchId) const {
    // By CAPABILITY, not by type (M17.10, ADR-M17-033). This loop enumerated
    // Pad alone until a review found it -- after Pocket and Revolve had both
    // shipped -- which made this function's own contract ("empty is exactly the
    // condition under which the sketch can be deleted") false for two of the
    // three kinds, and a loaded gun for the UI's delete path. Adding two
    // branches left the same trap set for the fourth kind; asking the feature
    // cannot go out of date.
    std::vector<ObjectId> ids;
    if (sketchId == kInvalidObjectId) return ids;
    for (const std::unique_ptr<Body>& body : bodies_)
        for (const std::unique_ptr<Feature>& feature : body->features()) {
            const auto* consumer = dynamic_cast<const ISketchConsuming*>(feature.get());
            if (consumer == nullptr) continue; // Fillet, Chamfer: no sketch at all
            // EVERY sketch it reads, not just its primary one. A loft's
            // second and third sections, a sweep's path and a curve
            // pattern's path were all invisible here -- so this function's
            // own contract ("empty is exactly when the sketch can be
            // deleted") was false for each of them, which is the same trap
            // it was written to close one level up.
            if (consumer->reads(sketchId)) ids.push_back(feature->id());
        }
    return ids;
}

bool PartDocument::markSketchDirty(ObjectId sketchId) {
    if (findSketch(sketchId) == nullptr) return false;
    if (!graph_.markDirty(sketchId)) return false;
    syncFeatureStatesFromGraph();
    return true;
}

// --- Pad feature (M4) ------------------------------------------------------

void PartDocument::wirePadFeature(PadFeature& feature, ObjectId sketchId,
                                  ObjectId lengthParameterId, ObjectId materialId) {
    addRecomputableNode(feature); // registry + graph node (IRecomputable*)
    addDependency(feature.id(), sketchId);          // Sketch -> Pad
    addDependency(feature.id(), lengthParameterId); // Length -> Pad
    rewireMassPropertiesSource(feature.id(), materialId);
}

PadFeature& PartDocument::addPadFeature(Body& body, std::string name, ObjectId sketchId,
                                        ObjectId lengthParameterId) {
    const ObjectId materialId = material_ ? material_->id() : kInvalidObjectId;
    PadFeature& feature =
        body.addFeature<PadFeature>(std::move(name), sketchId, lengthParameterId, materialId);
    wirePadFeature(feature, sketchId, lengthParameterId, materialId);
    recordFeatureAdded(body, feature);
    return feature;
}

PadFeature& PartDocument::restorePadFeature(Body& body, ObjectId id, std::string name,
                                            ComputeState state, ObjectId sketchId,
                                            ObjectId lengthParameterId, ObjectId materialId) {
    requireUnusedId(id, "restorePadFeature");
    PadFeature& feature = body.addFeature<PadFeature>(id, std::move(name), state, sketchId,
                                                      lengthParameterId, materialId);
    wirePadFeature(feature, sketchId, lengthParameterId, materialId);
    return feature;
}


PlaceholderFeature& PartDocument::addPlaceholderFeature(Body& body, std::string name,
                                                        std::string typeName) {
    return body.addFeature<PlaceholderFeature>(std::move(name), std::move(typeName));
}

PlaceholderFeature& PartDocument::restorePlaceholderFeature(Body& body, ObjectId id,
                                                            std::string name,
                                                            ComputeState state,
                                                            std::string typeName) {
    // The SEVENTH restore path gets the same duplicate-id guard as the other
    // six (ADR-M5-018) -- it shipped without one in round 2 and all three
    // round-3 reviewers independently demonstrated the consequence: a
    // colliding placeholder saved cleanly and the loader refused the bytes
    // (ADR-M3-008's class, fifth recurrence, introduced by a fix). TWO checks,
    // because placeholders are never registered: the registry catches a
    // collision with any registered object, and the feature scan catches a
    // collision with another placeholder -- which the registry cannot see,
    // the same blindness that let a placeholder-held id defeat the sibling
    // guards. Checked BEFORE addFeature, so a throw leaves no residue.
    requireUnusedId(id, "restorePlaceholderFeature");
    return body.addFeature<PlaceholderFeature>(id, std::move(name), state,
                                               std::move(typeName));
}

// --- Feature activity: suppression and rollback (M9.3 / M9.4) ---------------

bool PartDocument::isFeatureActive(ObjectId featureId) const noexcept {
    for (const std::unique_ptr<Body>& body : bodies_) {
        for (std::size_t i = 0; i < body->features().size(); ++i) {
            if (body->features()[i]->id() != featureId) continue;
            if (body->features()[i]->state() == ComputeState::Suppressed) return false;
            return !body->isRolledBack(i);
        }
    }
    // Not a feature of this document. Answering "active" keeps every caller's
    // fallback the pre-M9 behaviour: resolution then fails for the reason it
    // always did (the id is not a solid), rather than for a new one.
    return true;
}

ObjectId PartDocument::activeChainBase(ObjectId baseFeatureId) const noexcept {
    // Walks UP the chain past inactive links. Bounded by the number of features
    // in the document, because a cycle is unrepresentable (the loader and
    // `requireConsumableBase` both refuse one) but a bound costs nothing and an
    // unbounded walk over a corrupted document would hang rather than fail --
    // M9.1 learned that lesson from its own test.
    std::size_t features = 0;
    for (const std::unique_ptr<Body>& body : bodies_) features += body->features().size();

    ObjectId current = baseFeatureId;
    for (std::size_t step = 0; step <= features; ++step) {
        if (current == kInvalidObjectId) return kInvalidObjectId;
        if (isFeatureActive(current)) return current;
        // Inactive: fall through to what IT consumes.
        const ISolidFeature* solid = nullptr;
        for (const std::unique_ptr<Body>& body : bodies_)
            for (const std::unique_ptr<Feature>& feature : body->features())
                if (feature->id() == current)
                    solid = dynamic_cast<const ISolidFeature*>(feature.get());
        if (solid == nullptr) return kInvalidObjectId;
        current = solid->consumedSolidId();
    }
    return kInvalidObjectId;
}

bool PartDocument::restoreRollbackPosition(ObjectId bodyId, std::size_t cut) {
    const bool wasApplying = applyingHistory_;
    applyingHistory_ = true; // suppresses recording, exactly as undo does
    const bool ok = setRollbackPosition(bodyId, cut);
    applyingHistory_ = wasApplying;
    return ok;
}

bool PartDocument::setRollbackPosition(ObjectId bodyId, std::size_t cut) {
    Body* body = nullptr;
    for (const std::unique_ptr<Body>& candidate : bodies_)
        if (candidate->id() == bodyId) body = candidate.get();
    if (body == nullptr) return false;

    const std::size_t before = body->rollbackCut();
    body->setRollback(cut);
    const std::size_t after = body->rollbackCut();
    if (before == after) return true;

    RollbackEdit edit;
    edit.bodyId = bodyId;
    edit.before = before;
    edit.after = after;
    recordDelta(edit, "Roll back " + body->name());

    // Everything on either side of the move changes activity, so both the
    // features that just switched off and the ones that just switched on are
    // dirtied -- a feature coming back must recompute, and a feature going away
    // must stop being read as current.
    const std::size_t low = before < after ? before : after;
    const std::size_t high = before < after ? after : before;
    for (std::size_t i = low; i < high && i < body->features().size(); ++i)
        graph_.markDirty(body->features()[i]->id());
    // The tail moved. Mass follows it, or detaches when nothing is left.
    rewireMassPropertiesToTail(*body);
    syncFeatureStatesFromGraph();
    return true;
}

std::size_t PartDocument::rollbackPosition(ObjectId bodyId) const noexcept {
    for (const std::unique_ptr<Body>& body : bodies_)
        if (body->id() == bodyId) return body->rollbackCut();
    return 0;
}

// --- Undo / redo (M9.1, ADR-M9-001) -----------------------------------------

void PartDocument::recordFeatureAdded(const Body& body, const Feature& feature) {
    // recordDelta itself refuses while history is being applied, so redo --
    // which re-adds through the RESTORE facade, not through here -- cannot
    // double-record either way.
    FeatureExistenceEdit edit;
    edit.bodyId = body.id();
    edit.snapshot = SnapshotFeature(feature);
    edit.addedByTheEdit = true;
    edit.index = body.features().size() - 1; // just appended
    recordDelta(edit, "Add " + feature.name());
}



void PartDocument::applyOwnDelta(const UndoDelta& delta, bool forward) {
    // M12 -- sketch geometry and constraints.
    //
    // Both branches go through the SKETCH's restore path, not the add path, so
    // the entity or constraint comes back under the SAME id it had. An undo
    // that reissued ids would break every reference to it the moment anything
    // else survived the undo (A03), and it would make redo produce a document
    // the undo stack no longer describes.
    if (const auto* edit = std::get_if<SketchEntityExistenceEdit>(&delta)) {
        Sketch* sketch = findSketchForEdit(edit->sketchId);
        if (sketch == nullptr) return;
        const bool shouldExist = forward ? edit->addedByTheEdit : !edit->addedByTheEdit;
        const bool doesExist = sketch->findEntity(edit->entityId) != nullptr;
        if (shouldExist == doesExist) return;
        if (shouldExist) {
            (void)sketch->restoreEntity(edit->entityId, edit->geometry);
            // The flag comes back with it (M17.17). Without this, undoing and
            // redoing a polygon returns its circumscribed circle as a real
            // edge, and the next pad sweeps a curve nobody drew.
            if (edit->construction) sketch->setEntityConstruction(edit->entityId, true);
        } else {
            (void)sketch->removeEntityCascading(edit->entityId);
        }
        reconcileSketchParameterEdges(edit->sketchId);
        graph_.markDirty(edit->sketchId);
        syncFeatureStatesFromGraph();
        return;
    }
    if (const auto* edit = std::get_if<SketchDimensionFormatEdit>(&delta)) {
        Sketch* sketch = findSketchForEdit(edit->sketchId);
        if (sketch == nullptr) return;
        Sketch::DimensionFormat format;
        format.constraintId = edit->constraintId;
        format.prefix = forward ? edit->afterPrefix : edit->beforePrefix;
        format.suffix = forward ? edit->afterSuffix : edit->beforeSuffix;
        format.plusTolerance = forward ? edit->afterPlus : edit->beforePlus;
        format.minusTolerance = forward ? edit->afterMinus : edit->beforeMinus;
        // restore, not set: the validating setter would refuse a format being
        // put back alongside its constraint within one undo record.
        if (format.isDefault()) {
            Sketch::DimensionFormat empty;
            empty.constraintId = edit->constraintId;
            (void)sketch->setDimensionFormat(edit->constraintId, empty);
        } else {
            sketch->restoreDimensionFormat(format);
        }
        return;
    }
    if (const auto* edit = std::get_if<ObjectNameEdit>(&delta)) {
        // Through the same private writer the facade uses, so undo cannot set
        // a name the facade would have refused -- and NOT dirtying anything: a
        // name has no geometric consequence, and marking the object dirty
        // would rebuild the whole chain below it to produce identical shapes.
        applyName(edit->objectId, forward ? edit->after : edit->before);
        return;
    }
    if (const auto* edit = std::get_if<SketchEntityGeometryEdit>(&delta)) {
        Sketch* sketch = findSketchForEdit(edit->sketchId);
        if (sketch == nullptr) return;
        (void)sketch->setEntityGeometry(edit->entityId, forward ? edit->after : edit->before);
        return;
    }
    if (const auto* edit = std::get_if<SketchConstraintDrivenEdit>(&delta)) {
        Sketch* sketch = findSketchForEdit(edit->sketchId);
        if (sketch == nullptr) return;
        (void)sketch->setConstraintDriven(edit->constraintId, forward ? edit->after : edit->before);
        graph_.markDirty(edit->sketchId);
        return;
    }
    if (const auto* edit = std::get_if<SketchEntityConstructionEdit>(&delta)) {
        Sketch* sketch = findSketchForEdit(edit->sketchId);
        if (sketch == nullptr) return;
        (void)sketch->setEntityConstruction(edit->entityId, forward ? edit->after : edit->before);
        return;
    }
    if (const auto* edit = std::get_if<SketchDimensionPlacementEdit>(&delta)) {
        Sketch* sketch = findSketchForEdit(edit->sketchId);
        if (sketch == nullptr) return;
        const bool wantValue = forward ? edit->hasAfter : edit->hasBefore;
        const Vec2 value = forward ? edit->after : edit->before;
        if (wantValue) {
            // restore, not set: the constraint is guaranteed present here, but
            // going through the validating setter would refuse a placement
            // being restored alongside its constraint in the same undo record.
            sketch->restoreDimensionPlacement(edit->constraintId, value);
        } else {
            (void)sketch->clearDimensionPlacement(edit->constraintId);
        }
        return;
    }
    if (const auto* edit = std::get_if<SketchConstraintExistenceEdit>(&delta)) {
        Sketch* sketch = findSketchForEdit(edit->sketchId);
        if (sketch == nullptr) return;
        const bool shouldExist = forward ? edit->addedByTheEdit : !edit->addedByTheEdit;
        const bool doesExist = sketch->findConstraint(edit->constraintId) != nullptr;
        if (shouldExist == doesExist) return;
        if (shouldExist) {
            (void)sketch->restoreConstraint(edit->constraintId, edit->data);
        } else {
            (void)sketch->removeConstraint(edit->constraintId);
        }
        reconcileSketchParameterEdges(edit->sketchId);
        graph_.markDirty(edit->sketchId);
        syncFeatureStatesFromGraph();
        return;
    }
    if (const auto* edit = std::get_if<ParameterValueEdit>(&delta)) {
        setParameterValue(edit->parameterId, forward ? edit->after : edit->before);
        setParameterExpression(edit->parameterId,
                               forward ? edit->expressionAfter : edit->expressionBefore);
        return;
    }
    if (const auto* edit = std::get_if<ConnectorExistenceEdit>(&delta)) {
        const bool shouldExist = forward ? edit->addedByTheEdit : !edit->addedByTheEdit;
        const bool doesExist = findConnector(edit->connectorId) != nullptr;
        if (shouldExist == doesExist) return;
        if (shouldExist)
            restoreConnector(edit->connectorId, edit->name,
                             static_cast<ConnectorRole>(edit->role), edit->frameId,
                             static_cast<ConnectorOwner>(edit->owner));
        else
            removeObject(edit->connectorId);
        return;
    }
    if (const auto* edit = std::get_if<SketchSupportEdit>(&delta)) {
        setSketchSupportFrame(edit->sketchId, forward ? edit->after : edit->before);
        return;
    }
    if (const auto* edit = std::get_if<FrameTransformEdit>(&delta)) {
        setFrameTransform(edit->frameId, forward ? edit->after : edit->before);
        return;
    }
    if (const auto* edit = std::get_if<FrameParentEdit>(&delta)) {
        setFrameParent(edit->frameId, forward ? edit->after : edit->before);
        return;
    }
    if (const auto* edit = std::get_if<FrameExistenceEdit>(&delta)) {
        const bool shouldExist = forward ? edit->addedByTheEdit : !edit->addedByTheEdit;
        const bool doesExist = findFrame(edit->frameId) != nullptr;
        if (shouldExist == doesExist) return;
        if (shouldExist)
            restoreFrame(edit->frameId, edit->name, edit->parentFrameId, edit->localTransform);
        else
            removeObject(edit->frameId);
        return;
    }
    if (const auto* edit = std::get_if<ParameterExistenceEdit>(&delta)) {
        const bool shouldExist = forward ? edit->addedByTheEdit : !edit->addedByTheEdit;
        const bool doesExist = registry_.contains(edit->parameterId);
        if (shouldExist == doesExist) return;
        if (shouldExist)
            restoreParameter(edit->parameterId, edit->name, edit->value,
                             static_cast<UnitType>(edit->unit), edit->expression,
                             ParameterState::Dirty);
        else
            removeObject(edit->parameterId);
        return;
    }
    if (const auto* edit = std::get_if<SuppressionEdit>(&delta)) {
        setSuppressed(edit->featureId, forward ? edit->after : edit->before);
        return;
    }
    if (const auto* edit = std::get_if<RollbackEdit>(&delta)) {
        setRollbackPosition(edit->bodyId, forward ? edit->after : edit->before);
        return;
    }
    const auto& edit = std::get<FeatureExistenceEdit>(delta);
    Body* body = nullptr;
    for (const std::unique_ptr<Body>& candidate : bodies_)
        if (candidate->id() == edit.bodyId) body = candidate.get();
    if (body == nullptr) return; // the body is gone; nothing faithful to do

    const bool shouldExist = forward ? edit.addedByTheEdit : !edit.addedByTheEdit;
    bool doesExist = false;
    for (const std::unique_ptr<Feature>& feature : body->features())
        if (feature->id() == edit.snapshot.id) doesExist = true;
    if (shouldExist == doesExist) return;

    if (shouldExist) {
        // Rebuilt through the ordinary restore facade, so the registry entry,
        // the graph node and every dependency edge are wired exactly as they
        // are on load -- and with the ORIGINAL ObjectId, which is the whole
        // point: an undone deletion must give back the object the user had,
        // not a lookalike with a new identity that every other reference in
        // the document would no longer match.
        Feature& restored = RestoreFeatureFromSnapshot(*this, *body, edit.snapshot);
        // Back to the position it held. Order is load-bearing -- a consumer
        // must follow its base in the array or the document cannot be saved.
        body->moveFeatureToIndex(&restored, edit.index);
    } else {
        removeObject(edit.snapshot.id);
    }
    rewireMassPropertiesToTail(*body);
}


void PartDocument::rewireMassPropertiesToTail(const Body& body) {
    // The TAIL is the last solid feature nothing else consumes (ADR-M8-003).
    // Asked by capability, never by concrete type (ADR-M3-007).
    //
    // M8 recorded "removeObject does not re-point mass at the new tail" as a
    // known limitation; M9 gate C requires the re-point, so this is where it
    // lands.
    //
    // ACTIVITY, on both sides (M9.3/M9.4). An inactive consumer does not
    // consume -- suppress the Pocket and the Pad becomes the tail again, which
    // is the whole visible effect of suppressing it. And an inactive feature is
    // never itself the tail. Both directions matter: skipping only one of them
    // leaves either a body with no tail at all or a tail nobody is computing.
    std::unordered_set<ObjectId> consumed;
    for (const std::unique_ptr<Body>& anyBody : bodies_)
        for (const std::unique_ptr<Feature>& feature : anyBody->features()) {
            if (!isFeatureActive(feature->id())) continue;
            if (const auto* solid = dynamic_cast<const ISolidFeature*>(feature.get()))
                // EVERY one it consumes (M21). A boolean eats two, and the one
                // this loop missed would stay a live chain tail -- so the
                // viewer would draw the leftover alongside the result and the
                // part would appear twice.
                for (const ObjectId eaten : solid->consumedSolidIds())
                    if (eaten != kInvalidObjectId) consumed.insert(activeChainBase(eaten));
        }

    const Feature* tail = nullptr;
    for (std::size_t i = 0; i < body.features().size(); ++i) {
        const Feature* feature = body.features()[i].get();
        if (dynamic_cast<const ISolidFeature*>(feature) == nullptr) continue;
        if (!isFeatureActive(feature->id())) continue;
        if (consumed.count(feature->id()) != 0) continue;
        tail = feature;
    }
    if (tail == nullptr) {
        // Nothing solid left in this body: detach rather than point at a
        // destroyed id, and stop reporting the last numbers as current.
        massPropertiesNode_.setSource(kInvalidObjectId, massPropertiesNode_.materialId());
        graph_.removeNode(massPropertiesNode_.id());
        massProperties_ = MassProperties{};
        return;
    }
    ObjectId materialId = massPropertiesNode_.materialId();
    if (const auto* referencing = dynamic_cast<const IMaterialReferencing*>(tail))
        materialId = referencing->materialId();
    rewireMassPropertiesSource(tail->id(), materialId);
}


void PartDocument::requireUnusedIdHook(ObjectId id, const char* who) const {
    // Features the registry cannot see. PlaceholderFeature is deliberately
    // never registered (ADR-009 D4), so without this scan a placeholder-held
    // id passes the base's registry check and collides anyway -- which is
    // exactly how round 4's R1R4-C1 built two features with one ObjectId in
    // one Body through public calls alone.
    for (const auto& anyBody : bodies_)
        for (const auto& feature : anyBody->features())
            if (feature->id() == id)
                throw std::runtime_error(std::string(who) + ": id " + std::to_string(id) +
                                         " is already used by a feature in this document");
}

void PartDocument::requireConsumableBase(const Body& body, ObjectId baseFeatureId,
                                         const char* consumerNoun) const {
    const ISolidFeature* baseInBody = nullptr;
    for (const auto& feature : body.features())
        if (feature->id() == baseFeatureId) {
            baseInBody = dynamic_cast<const ISolidFeature*>(feature.get());
            break; // ids are unique, so the first match is the only match
                   // (round 4, R1R4-m2: taking the LAST match meant a
                   // duplicate-id body -- which requireUnusedId now makes
                   // unconstructible -- would have been judged on the wrong
                   // feature).
        }
    if (baseInBody == nullptr)
        throw std::runtime_error(std::string(consumerNoun) + ": base feature " +
                                 std::to_string(baseFeatureId) +
                                 " is not a solid feature of the same body");
    // Consumption is document-wide and unique: once ANY consumer (in any
    // body -- review probe R1-PROBE3 reached a cross-body consumer before the
    // same-body check above existed) reports this base via consumedSolidId(),
    // it is an intermediate value, never a base again. Capability, not
    // concrete types (ADR-M3-007).
    for (const auto& anyBody : bodies_)
        for (const auto& feature : anyBody->features()) {
            const auto* solid = dynamic_cast<const ISolidFeature*>(feature.get());
            bool eatsIt = false;
            if (solid != nullptr)
                for (const ObjectId eaten : solid->consumedSolidIds())
                    if (eaten == baseFeatureId) eatsIt = true;
            if (eatsIt)
                throw std::runtime_error(std::string(consumerNoun) + ": base feature " +
                                         std::to_string(baseFeatureId) +
                                         " is already consumed by feature " +
                                         std::to_string(feature->id()) +
                                         "; a solid may be consumed once (ADR-M8-008)");
        }
}

void PartDocument::wirePocketFeature(PocketFeature& feature, ObjectId baseFeatureId,
                                     ObjectId sketchId, ObjectId depthParameterId,
                                     ObjectId materialId) {
    addRecomputableNode(feature); // registry + graph node (IRecomputable*)
    // THE chain edge (ADR-M8-001): editing anything that rebuilds the base
    // dirties the pocket through the ordinary M2 machinery. Without it, a
    // Width edit would rebuild the pad and leave the pocket cutting yesterday's
    // solid -- current-looking, analytically wrong.
    //
    // The GraphResult is CHECKED (round 1's R2-M2): a silently discarded
    // failed edge is exactly the "consumer with no chain edge" state
    // ADR-M8-001 promises is unrepresentable. Reachable only by calling the
    // restore facade directly with a base the loader would have refused.
    const GraphResult baseEdge = addDependency(feature.id(), baseFeatureId); // Base -> Pocket
    if (!baseEdge)
        throw std::runtime_error("wirePocketFeature: base feature " +
                                 std::to_string(baseFeatureId) +
                                 " has no graph node; the chain edge cannot be wired");
    addDependency(feature.id(), sketchId);          // Sketch -> Pocket
    addDependency(feature.id(), depthParameterId);  // Depth  -> Pocket
    // Physics follows the chain TAIL (ADR-M8-003): the pocketed result is the
    // part; the pad alone is now an intermediate value.
    rewireMassPropertiesSource(feature.id(), materialId);
}

PocketFeature& PartDocument::addPocketFeature(Body& body, std::string name,
                                              ObjectId baseFeatureId, ObjectId sketchId,
                                              ObjectId depthParameterId) {
    requireConsumableBase(body, baseFeatureId, "addPocketFeature");
    const ObjectId materialId = material_ ? material_->id() : kInvalidObjectId;
    PocketFeature& feature = body.addFeature<PocketFeature>(
        std::move(name), baseFeatureId, sketchId, depthParameterId, materialId);
    wirePocketFeature(feature, baseFeatureId, sketchId, depthParameterId, materialId);
    recordFeatureAdded(body, feature);
    return feature;
}

PocketFeature& PartDocument::restorePocketFeature(Body& body, ObjectId id, std::string name,
                                                  ComputeState state, ObjectId baseFeatureId,
                                                  ObjectId sketchId, ObjectId depthParameterId,
                                                  ObjectId materialId) {
    // Same duplicate-id guard as every restored type (ADR-M5-018, and the M6
    // review that found the types the fix skipped).
    requireUnusedId(id, "restorePocketFeature");
    requireConsumableBase(body, baseFeatureId, "restorePocketFeature");
    PocketFeature& feature = body.addFeature<PocketFeature>(
        id, std::move(name), state, baseFeatureId, sketchId, depthParameterId, materialId);
    wirePocketFeature(feature, baseFeatureId, sketchId, depthParameterId, materialId);
    return feature;
}


void PartDocument::wireImportFeature(ImportFeature& feature, ObjectId materialId) {
    addRecomputableNode(feature);
    // NO DEPENDENCY EDGES. An import's input is a FILE, and the graph tracks
    // objects in this document -- it has no node for something outside it.
    //
    // What that costs is honest and worth stating: editing the STEP file does
    // not dirty this feature, so the model does not follow it until something
    // else asks for a rebuild. A file watcher would be a second thing that has
    // to agree with the graph about when to recompute, and this milestone did
    // not buy one.
    rewireMassPropertiesSource(feature.id(), materialId);
}

ImportFeature& PartDocument::addImportFeature(Body& body, std::string name, std::string path) {
    const ObjectId materialId = material_ ? material_->id() : kInvalidObjectId;
    ImportFeature& feature =
        body.addFeature<ImportFeature>(std::move(name), std::move(path), materialId);
    wireImportFeature(feature, materialId);
    recordFeatureAdded(body, feature);
    return feature;
}

ImportFeature& PartDocument::restoreImportFeature(Body& body, ObjectId id, std::string name,
                                                  ComputeState state, std::string path,
                                                  ObjectId materialId) {
    requireUnusedId(id, "restoreImportFeature");
    ImportFeature& feature = body.addFeature<ImportFeature>(id, std::move(name), state,
                                                            std::move(path), materialId);
    wireImportFeature(feature, materialId);
    return feature;
}

void PartDocument::wireBooleanFeature(BooleanFeature& feature, ObjectId targetFeatureId,
                                      ObjectId toolFeatureId, ObjectId materialId) {
    addRecomputableNode(feature);
    // BOTH operands. Either one changing changes the result, so either one
    // has to dirty this -- the same reason a sweep needs two edges.
    addDependency(feature.id(), targetFeatureId);
    addDependency(feature.id(), toolFeatureId);
    rewireMassPropertiesSource(feature.id(), materialId);
}

BooleanFeature& PartDocument::addBooleanFeature(Body& body, std::string name,
                                                BooleanOperation operation,
                                                ObjectId targetFeatureId,
                                                ObjectId toolFeatureId) {
    // BOTH operands, because a boolean eats both -- and the rule is about
    // what gets eaten, not about how many arguments a feature takes.
    requireConsumableBase(body, targetFeatureId, "addBooleanFeature");
    requireConsumableBase(body, toolFeatureId, "addBooleanFeature");
    const ObjectId materialId = material_ ? material_->id() : kInvalidObjectId;
    BooleanFeature& feature = body.addFeature<BooleanFeature>(
        std::move(name), operation, targetFeatureId, toolFeatureId, materialId);
    wireBooleanFeature(feature, targetFeatureId, toolFeatureId, materialId);
    recordFeatureAdded(body, feature);
    return feature;
}

BooleanFeature& PartDocument::restoreBooleanFeature(Body& body, ObjectId id, std::string name,
                                                    ComputeState state,
                                                    BooleanOperation operation,
                                                    ObjectId targetFeatureId,
                                                    ObjectId toolFeatureId,
                                                    ObjectId materialId) {
    requireConsumableBase(body, targetFeatureId, "restoreBooleanFeature");
    requireConsumableBase(body, toolFeatureId, "restoreBooleanFeature");
    requireUnusedId(id, "restoreBooleanFeature");
    BooleanFeature& feature = body.addFeature<BooleanFeature>(
        id, std::move(name), state, operation, targetFeatureId, toolFeatureId, materialId);
    wireBooleanFeature(feature, targetFeatureId, toolFeatureId, materialId);
    return feature;
}

CircularPatternFeature& PartDocument::addCircularPatternFeature(Body& body, std::string name,
                                                                ObjectId baseFeatureId,
                                                                ObjectId frameId,
                                                                ObjectId countParameterId,
                                                                ObjectId stepParameterId) {
    requireConsumableBase(body, baseFeatureId, "addCircularPatternFeature");
    const ObjectId materialId = material_ ? material_->id() : kInvalidObjectId;
    CircularPatternFeature& feature = body.addFeature<CircularPatternFeature>(
        std::move(name), baseFeatureId, frameId, countParameterId, stepParameterId, materialId);
    // The SAME wiring a linear pattern uses -- base, frame, count and the one
    // driving number -- so the two cannot drift apart in what they depend on.
    wireTransformFeature(feature, baseFeatureId, frameId, countParameterId, stepParameterId,
                         materialId);
    recordFeatureAdded(body, feature);
    return feature;
}

CircularPatternFeature& PartDocument::restoreCircularPatternFeature(
    Body& body, ObjectId id, std::string name, ComputeState state, ObjectId baseFeatureId,
    ObjectId frameId, ObjectId countParameterId, ObjectId stepParameterId, ObjectId materialId) {
    requireUnusedId(id, "restoreCircularPatternFeature");
    CircularPatternFeature& feature = body.addFeature<CircularPatternFeature>(
        id, std::move(name), state, baseFeatureId, frameId, countParameterId, stepParameterId,
        materialId);
    wireTransformFeature(feature, baseFeatureId, frameId, countParameterId, stepParameterId,
                         materialId);
    return feature;
}

void PartDocument::wireCurvePatternFeature(CurvePatternFeature& feature, ObjectId baseFeatureId,
                                           ObjectId pathSketchId, ObjectId countParameterId,
                                           ObjectId materialId) {
    addRecomputableNode(feature);
    addDependency(feature.id(), baseFeatureId);
    // THE PATH SKETCH, because moving the curve moves every copy.
    addDependency(feature.id(), pathSketchId);
    addDependency(feature.id(), countParameterId);
    rewireMassPropertiesSource(feature.id(), materialId);
}

CurvePatternFeature& PartDocument::addCurvePatternFeature(Body& body, std::string name,
                                                          ObjectId baseFeatureId,
                                                          ObjectId pathSketchId,
                                                          ObjectId countParameterId) {
    requireConsumableBase(body, baseFeatureId, "addCurvePatternFeature");
    const ObjectId materialId = material_ ? material_->id() : kInvalidObjectId;
    CurvePatternFeature& feature = body.addFeature<CurvePatternFeature>(
        std::move(name), baseFeatureId, pathSketchId, countParameterId, materialId);
    wireCurvePatternFeature(feature, baseFeatureId, pathSketchId, countParameterId, materialId);
    recordFeatureAdded(body, feature);
    return feature;
}

CurvePatternFeature& PartDocument::restoreCurvePatternFeature(Body& body, ObjectId id,
                                                              std::string name,
                                                              ComputeState state,
                                                              ObjectId baseFeatureId,
                                                              ObjectId pathSketchId,
                                                              ObjectId countParameterId,
                                                              ObjectId materialId) {
    requireUnusedId(id, "restoreCurvePatternFeature");
    CurvePatternFeature& feature = body.addFeature<CurvePatternFeature>(
        id, std::move(name), state, baseFeatureId, pathSketchId, countParameterId, materialId);
    wireCurvePatternFeature(feature, baseFeatureId, pathSketchId, countParameterId, materialId);
    return feature;
}

void PartDocument::wireShellFeature(ShellFeature& feature, ObjectId baseFeatureId,
                                    ObjectId thicknessParameterId, ObjectId materialId) {
    addRecomputableNode(feature);
    addDependency(feature.id(), baseFeatureId);
    addDependency(feature.id(), thicknessParameterId);
    // The face SELECTION needs no edge of its own: the faces belong to the
    // base, so anything that can move them dirties the base this already
    // depends on. A second edge would say the same thing twice -- the reason
    // a revolve's axis has none either.
    rewireMassPropertiesSource(feature.id(), materialId);
}

ShellFeature& PartDocument::addShellFeature(Body& body, std::string name, ObjectId baseFeatureId,
                                            FaceSelection openFaces,
                                            ObjectId thicknessParameterId) {
    requireConsumableBase(body, baseFeatureId, "addShellFeature");
    const ObjectId materialId = material_ ? material_->id() : kInvalidObjectId;
    ShellFeature& feature = body.addFeature<ShellFeature>(
        std::move(name), baseFeatureId, std::move(openFaces), thicknessParameterId, materialId);
    wireShellFeature(feature, baseFeatureId, thicknessParameterId, materialId);
    recordFeatureAdded(body, feature);
    return feature;
}

ShellFeature& PartDocument::restoreShellFeature(Body& body, ObjectId id, std::string name,
                                                ComputeState state, ObjectId baseFeatureId,
                                                FaceSelection openFaces,
                                                ObjectId thicknessParameterId,
                                                ObjectId materialId) {
    requireUnusedId(id, "restoreShellFeature");
    ShellFeature& feature =
        body.addFeature<ShellFeature>(id, std::move(name), state, baseFeatureId,
                                      std::move(openFaces), thicknessParameterId, materialId);
    wireShellFeature(feature, baseFeatureId, thicknessParameterId, materialId);
    return feature;
}

void PartDocument::wireDraftFeature(DraftFeature& feature, ObjectId baseFeatureId,
                                    ObjectId angleParameterId, ObjectId materialId) {
    addRecomputableNode(feature);
    addDependency(feature.id(), baseFeatureId);
    addDependency(feature.id(), angleParameterId);
    rewireMassPropertiesSource(feature.id(), materialId);
}

DraftFeature& PartDocument::addDraftFeature(Body& body, std::string name, ObjectId baseFeatureId,
                                            FaceSelection faces, FaceQuery neutral,
                                            ObjectId angleParameterId) {
    requireConsumableBase(body, baseFeatureId, "addDraftFeature");
    const ObjectId materialId = material_ ? material_->id() : kInvalidObjectId;
    DraftFeature& feature =
        body.addFeature<DraftFeature>(std::move(name), baseFeatureId, std::move(faces),
                                      std::move(neutral), angleParameterId, materialId);
    wireDraftFeature(feature, baseFeatureId, angleParameterId, materialId);
    recordFeatureAdded(body, feature);
    return feature;
}

DraftFeature& PartDocument::restoreDraftFeature(Body& body, ObjectId id, std::string name,
                                                ComputeState state, ObjectId baseFeatureId,
                                                FaceSelection faces, FaceQuery neutral,
                                                ObjectId angleParameterId, ObjectId materialId) {
    requireUnusedId(id, "restoreDraftFeature");
    DraftFeature& feature = body.addFeature<DraftFeature>(id, std::move(name), state,
                                                          baseFeatureId, std::move(faces),
                                                          std::move(neutral), angleParameterId,
                                                          materialId);
    wireDraftFeature(feature, baseFeatureId, angleParameterId, materialId);
    return feature;
}

void PartDocument::wireHoleFeature(HoleFeature& feature, ObjectId baseFeatureId,
                                   ObjectId sketchId, ObjectId diameterParameterId,
                                   ObjectId depthParameterId, ObjectId materialId) {
    addRecomputableNode(feature);
    // FOUR EDGES: the solid it drills, the sketch that says where, and the two
    // numbers that say how big and how deep. Every one of them changes the
    // result, so every one of them has to dirty this feature.
    addDependency(feature.id(), baseFeatureId);
    addDependency(feature.id(), sketchId);
    addDependency(feature.id(), diameterParameterId);
    addDependency(feature.id(), depthParameterId);
    rewireMassPropertiesSource(feature.id(), materialId);
}

HoleFeature& PartDocument::addHoleFeature(Body& body, std::string name, ObjectId baseFeatureId,
                                          ObjectId sketchId, ObjectId diameterParameterId,
                                          ObjectId depthParameterId) {
    requireConsumableBase(body, baseFeatureId, "addHoleFeature");
    const ObjectId materialId = material_ ? material_->id() : kInvalidObjectId;
    HoleFeature& feature = body.addFeature<HoleFeature>(
        std::move(name), baseFeatureId, sketchId, diameterParameterId, depthParameterId,
        materialId);
    wireHoleFeature(feature, baseFeatureId, sketchId, diameterParameterId, depthParameterId,
                    materialId);
    recordFeatureAdded(body, feature);
    return feature;
}

HoleFeature& PartDocument::restoreHoleFeature(Body& body, ObjectId id, std::string name,
                                              ComputeState state, ObjectId baseFeatureId,
                                              ObjectId sketchId, ObjectId diameterParameterId,
                                              ObjectId depthParameterId, ObjectId materialId) {
    requireUnusedId(id, "restoreHoleFeature");
    HoleFeature& feature = body.addFeature<HoleFeature>(id, std::move(name), state, baseFeatureId,
                                                        sketchId, diameterParameterId,
                                                        depthParameterId, materialId);
    wireHoleFeature(feature, baseFeatureId, sketchId, diameterParameterId, depthParameterId,
                    materialId);
    return feature;
}

void PartDocument::wireSweepFeature(SweepFeature& feature, ObjectId profileSketchId,
                                    ObjectId pathSketchId, ObjectId materialId) {
    addRecomputableNode(feature);
    // BOTH SKETCHES. Moving the spine changes the solid exactly as much as
    // moving the section does, so an edge from only one would leave the feature
    // clean after an edit that changed its shape -- and it would stay wrong
    // until something unrelated dirtied it.
    addDependency(feature.id(), profileSketchId);
    addDependency(feature.id(), pathSketchId);
    rewireMassPropertiesSource(feature.id(), materialId);
}

SweepFeature& PartDocument::addSweepFeature(Body& body, std::string name,
                                            ObjectId profileSketchId, ObjectId pathSketchId) {
    const ObjectId materialId = material_ ? material_->id() : kInvalidObjectId;
    SweepFeature& feature =
        body.addFeature<SweepFeature>(std::move(name), profileSketchId, pathSketchId, materialId);
    wireSweepFeature(feature, profileSketchId, pathSketchId, materialId);
    recordFeatureAdded(body, feature);
    return feature;
}

SweepFeature& PartDocument::restoreSweepFeature(Body& body, ObjectId id, std::string name,
                                                ComputeState state, ObjectId profileSketchId,
                                                ObjectId pathSketchId, ObjectId materialId) {
    requireUnusedId(id, "restoreSweepFeature");
    SweepFeature& feature = body.addFeature<SweepFeature>(
        id, std::move(name), state, profileSketchId, pathSketchId, materialId);
    wireSweepFeature(feature, profileSketchId, pathSketchId, materialId);
    return feature;
}

void PartDocument::wireLoftFeature(LoftFeature& feature,
                                   const std::vector<ObjectId>& sectionSketchIds,
                                   ObjectId materialId) {
    addRecomputableNode(feature);
    // EVERY SECTION. A loft's shape is a function of all of them, so all of
    // them are dependencies -- the same reason a sweep needs two edges rather
    // than one.
    for (const ObjectId sketchId : sectionSketchIds) addDependency(feature.id(), sketchId);
    rewireMassPropertiesSource(feature.id(), materialId);
}

LoftFeature& PartDocument::addLoftFeature(Body& body, std::string name,
                                          std::vector<ObjectId> sectionSketchIds) {
    const ObjectId materialId = material_ ? material_->id() : kInvalidObjectId;
    LoftFeature& feature =
        body.addFeature<LoftFeature>(std::move(name), sectionSketchIds, materialId);
    wireLoftFeature(feature, sectionSketchIds, materialId);
    recordFeatureAdded(body, feature);
    return feature;
}

LoftFeature& PartDocument::restoreLoftFeature(Body& body, ObjectId id, std::string name,
                                              ComputeState state,
                                              std::vector<ObjectId> sectionSketchIds,
                                              ObjectId materialId) {
    requireUnusedId(id, "restoreLoftFeature");
    LoftFeature& feature = body.addFeature<LoftFeature>(id, std::move(name), state,
                                                        sectionSketchIds, materialId);
    wireLoftFeature(feature, sectionSketchIds, materialId);
    return feature;
}

void PartDocument::wireRevolveFeature(RevolveFeature& feature, ObjectId sketchId,
                                      ObjectId angleParameterId, ObjectId materialId) {
    addRecomputableNode(feature); // registry + graph node (IRecomputable*)
    addDependency(feature.id(), sketchId);           // Sketch -> Revolve
    addDependency(feature.id(), angleParameterId);   // Angle  -> Revolve
    // The AXIS needs no edge of its own: it is an entity OF the sketch, so any
    // edit that can move it arrives through editSketch, which already dirties
    // the sketch node this feature depends on. A second edge would say the
    // same thing twice.
    rewireMassPropertiesSource(feature.id(), materialId);
}

RevolveFeature& PartDocument::addRevolveFeature(Body& body, std::string name, ObjectId sketchId,
                                                SketchEntityId axisEntityId,
                                                ObjectId angleParameterId) {
    const ObjectId materialId = material_ ? material_->id() : kInvalidObjectId;
    RevolveFeature& feature = body.addFeature<RevolveFeature>(
        std::move(name), sketchId, axisEntityId, angleParameterId, materialId);
    wireRevolveFeature(feature, sketchId, angleParameterId, materialId);
    recordFeatureAdded(body, feature);
    return feature;
}

RevolveFeature& PartDocument::restoreRevolveFeature(Body& body, ObjectId id, std::string name,
                                                    ComputeState state, ObjectId sketchId,
                                                    SketchEntityId axisEntityId,
                                                    ObjectId angleParameterId,
                                                    ObjectId materialId) {
    requireUnusedId(id, "restoreRevolveFeature");
    RevolveFeature& feature = body.addFeature<RevolveFeature>(
        id, std::move(name), state, sketchId, axisEntityId, angleParameterId, materialId);
    wireRevolveFeature(feature, sketchId, angleParameterId, materialId);
    return feature;
}


void PartDocument::wireTransformFeature(TransformFeature& feature, ObjectId baseFeatureId,
                                        ObjectId frameId, ObjectId countParameterId,
                                        ObjectId spacingParameterId, ObjectId materialId) {
    addRecomputableNode(feature);
    // The chain edge, checked like every other (round 2's R2-M2: a discarded
    // GraphResult is the unrepresentable state ADR-M8-001 promises not to have).
    if (!addDependency(feature.id(), baseFeatureId))
        throw std::runtime_error("wireTransformFeature: base edge " +
                                 std::to_string(baseFeatureId) + " could not be wired");
    // The FRAME edge is what makes a mirror parametric: move the frame and the
    // mirrored half moves, through the same machinery a Parameter edit uses.
    if (!addDependency(feature.id(), frameId))
        throw std::runtime_error("wireTransformFeature: frame edge " + std::to_string(frameId) +
                                 " could not be wired");
    if (countParameterId != kInvalidObjectId) addDependency(feature.id(), countParameterId);
    if (spacingParameterId != kInvalidObjectId) addDependency(feature.id(), spacingParameterId);
    rewireMassPropertiesSource(feature.id(), materialId);
}

MirrorFeature& PartDocument::addMirrorFeature(Body& body, std::string name,
                                              ObjectId baseFeatureId, ObjectId frameId) {
    requireConsumableBase(body, baseFeatureId, "addMirrorFeature");
    if (findFrame(frameId) == nullptr)
        throw std::runtime_error("addMirrorFeature: frame " + std::to_string(frameId) +
                                 " is not a reference frame in this document");
    const ObjectId materialId = material_ ? material_->id() : kInvalidObjectId;
    MirrorFeature& feature =
        body.addFeature<MirrorFeature>(std::move(name), baseFeatureId, frameId, materialId);
    wireTransformFeature(feature, baseFeatureId, frameId, kInvalidObjectId, kInvalidObjectId,
                         materialId);
    recordFeatureAdded(body, feature);
    return feature;
}

MirrorFeature& PartDocument::restoreMirrorFeature(Body& body, ObjectId id, std::string name,
                                                  ComputeState state, ObjectId baseFeatureId,
                                                  ObjectId frameId, ObjectId materialId) {
    requireUnusedId(id, "restoreMirrorFeature");
    requireConsumableBase(body, baseFeatureId, "restoreMirrorFeature");
    MirrorFeature& feature = body.addFeature<MirrorFeature>(id, std::move(name), state,
                                                            baseFeatureId, frameId, materialId);
    wireTransformFeature(feature, baseFeatureId, frameId, kInvalidObjectId, kInvalidObjectId,
                         materialId);
    return feature;
}

PatternFeature& PartDocument::addPatternFeature(Body& body, std::string name,
                                                ObjectId baseFeatureId, ObjectId frameId,
                                                ObjectId countParameterId,
                                                ObjectId spacingParameterId) {
    requireConsumableBase(body, baseFeatureId, "addPatternFeature");
    if (findFrame(frameId) == nullptr)
        throw std::runtime_error("addPatternFeature: frame " + std::to_string(frameId) +
                                 " is not a reference frame in this document");
    const ObjectId materialId = material_ ? material_->id() : kInvalidObjectId;
    PatternFeature& feature = body.addFeature<PatternFeature>(
        std::move(name), baseFeatureId, frameId, countParameterId, spacingParameterId,
        materialId);
    wireTransformFeature(feature, baseFeatureId, frameId, countParameterId, spacingParameterId,
                         materialId);
    recordFeatureAdded(body, feature);
    return feature;
}

PatternFeature& PartDocument::restorePatternFeature(Body& body, ObjectId id, std::string name,
                                                    ComputeState state, ObjectId baseFeatureId,
                                                    ObjectId frameId, ObjectId countParameterId,
                                                    ObjectId spacingParameterId,
                                                    ObjectId materialId) {
    requireUnusedId(id, "restorePatternFeature");
    requireConsumableBase(body, baseFeatureId, "restorePatternFeature");
    PatternFeature& feature =
        body.addFeature<PatternFeature>(id, std::move(name), state, baseFeatureId, frameId,
                                        countParameterId, spacingParameterId, materialId);
    wireTransformFeature(feature, baseFeatureId, frameId, countParameterId, spacingParameterId,
                         materialId);
    return feature;
}

void PartDocument::wireEdgeDressFeature(EdgeDressFeature& feature, ObjectId baseFeatureId,
                                        ObjectId sizeParameterId, ObjectId materialId) {
    addRecomputableNode(feature);
    // Checked for the same reason as wirePocketFeature's base edge (R2-M2).
    const GraphResult baseEdge = addDependency(feature.id(), baseFeatureId); // Base -> dress
    if (!baseEdge)
        throw std::runtime_error("wireEdgeDressFeature: base feature " +
                                 std::to_string(baseFeatureId) +
                                 " has no graph node; the chain edge cannot be wired");
    addDependency(feature.id(), sizeParameterId);  // Size -> Fillet/Chamfer
    rewireMassPropertiesSource(feature.id(), materialId); // chain tail
}

FilletFeature& PartDocument::addFilletFeature(Body& body, std::string name,
                                              ObjectId baseFeatureId,
                                              ObjectId radiusParameterId) {
    requireConsumableBase(body, baseFeatureId, "addFilletFeature");
    const ObjectId materialId = material_ ? material_->id() : kInvalidObjectId;
    FilletFeature& feature = body.addFeature<FilletFeature>(std::move(name), baseFeatureId,
                                                            radiusParameterId, materialId);
    wireEdgeDressFeature(feature, baseFeatureId, radiusParameterId, materialId);
    recordFeatureAdded(body, feature);
    return feature;
}

FilletFeature& PartDocument::restoreFilletFeature(Body& body, ObjectId id, std::string name,
                                                  ComputeState state, ObjectId baseFeatureId,
                                                  ObjectId radiusParameterId,
                                                  ObjectId materialId) {
    requireUnusedId(id, "restoreFilletFeature");
    requireConsumableBase(body, baseFeatureId, "restoreFilletFeature");
    FilletFeature& feature = body.addFeature<FilletFeature>(
        id, std::move(name), state, baseFeatureId, radiusParameterId, materialId);
    wireEdgeDressFeature(feature, baseFeatureId, radiusParameterId, materialId);
    return feature;
}

ChamferFeature& PartDocument::addChamferFeature(Body& body, std::string name,
                                                ObjectId baseFeatureId,
                                                ObjectId distanceParameterId) {
    requireConsumableBase(body, baseFeatureId, "addChamferFeature");
    const ObjectId materialId = material_ ? material_->id() : kInvalidObjectId;
    ChamferFeature& feature = body.addFeature<ChamferFeature>(std::move(name), baseFeatureId,
                                                              distanceParameterId, materialId);
    wireEdgeDressFeature(feature, baseFeatureId, distanceParameterId, materialId);
    recordFeatureAdded(body, feature);
    return feature;
}

ChamferFeature& PartDocument::restoreChamferFeature(Body& body, ObjectId id, std::string name,
                                                    ComputeState state, ObjectId baseFeatureId,
                                                    ObjectId distanceParameterId,
                                                    ObjectId materialId) {
    requireUnusedId(id, "restoreChamferFeature");
    requireConsumableBase(body, baseFeatureId, "restoreChamferFeature");
    ChamferFeature& feature = body.addFeature<ChamferFeature>(
        id, std::move(name), state, baseFeatureId, distanceParameterId, materialId);
    wireEdgeDressFeature(feature, baseFeatureId, distanceParameterId, materialId);
    return feature;
}

BoxFeature& PartDocument::addBoxFeature(Body& body, std::string name, ObjectId widthParameterId,
                                        ObjectId heightParameterId, ObjectId depthParameterId) {
    const ObjectId materialId = material_ ? material_->id() : kInvalidObjectId;
    BoxFeature& feature = body.addFeature<BoxFeature>(std::move(name), widthParameterId,
                                                       heightParameterId, depthParameterId,
                                                       materialId);
    wireBoxFeature(feature, widthParameterId, heightParameterId, depthParameterId, materialId);
    recordFeatureAdded(body, feature);
    return feature;
}

BoxFeature& PartDocument::restoreBoxFeature(Body& body, ObjectId id, std::string name,
                                            ComputeState state, ObjectId widthParameterId,
                                            ObjectId heightParameterId, ObjectId depthParameterId,
                                            ObjectId materialId) {
    // ADR-M5-018 said "every restored type" and meant four; there are six.
    // Without this, a duplicate feature id threw nothing, left two features
    // with the same id in one Body, and the document saved cleanly and reloaded
    // with "duplicate ObjectId" -- C2's symptom on the types the fix skipped.
    requireUnusedId(id, "restoreBoxFeature");
    BoxFeature& feature = body.addFeature<BoxFeature>(id, std::move(name), state,
                                                       widthParameterId, heightParameterId,
                                                       depthParameterId, materialId);
    wireBoxFeature(feature, widthParameterId, heightParameterId, depthParameterId, materialId);
    return feature;
}


bool PartDocument::markDirty(ObjectId id) {
    // The graph half is generic and lives in DocumentBase. This is the ADR-011
    // BRIDGE, which is not: ParameterState is a Part concept, and an Assembly
    // has no parameters to bridge to.
    if (!DocumentBase::markDirty(id)) return false;
    if (const ObjectRegistry::ObjectRef* ref = registry_.find(id))
        if (auto* const* parameter = std::get_if<Parameter*>(ref))
            (*parameter)->markEvaluationDirty();
    return true;
}

GraphResult PartDocument::setSuppressed(ObjectId id, bool suppressed) {
    // The graph node AND the feature's own ComputeState cache (round 4,
    // R1R4-m1). Setting only the graph left `ComputeState::Suppressed`
    // unobservable through the facade: `syncFeatureStatesFromGraph` can only
    // ever write Dirty (Feature::markDirty deliberately refuses to overwrite
    // Suppressed, and the feature was never PUT in that state), so the graph
    // said Suppressed while the feature -- the thing the model tree, the
    // property panel and every diagnostic read -- said Dirty. The one state a
    // user most needs to see was the one state the UI could not report.
    //
    // Feature first, then graph, so a NodeNotFound cannot leave the two
    // disagreeing in the other direction.
    Feature* feature = nullptr;
    for (const std::unique_ptr<Body>& body : bodies_)
        for (const std::unique_ptr<Feature>& candidate : body->features())
            if (candidate->id() == id) feature = candidate.get();

    const bool before = feature != nullptr && feature->state() == ComputeState::Suppressed;
    const GraphResult graphResult = graph_.setSuppressed(id, suppressed);
    if (feature == nullptr) return graphResult;
    feature->setSuppressed(suppressed);
    // Suppressing a chain member moves the tail (ADR-M9-002): the pad becomes
    // the displayed solid again the moment the pocket is switched off, and the
    // mass must follow it in the same step rather than after the next edit.
    for (const std::unique_ptr<Body>& body : bodies_)
        for (const std::unique_ptr<Feature>& candidate : body->features())
            if (candidate->id() == id) rewireMassPropertiesToTail(*body);
    // Everything downstream has to reconsider what it is built on.
    graph_.markDirty(id);
    if (suppressed != before) {
        SuppressionEdit edit;
        edit.featureId = id;
        edit.before = before;
        edit.after = suppressed;
        recordDelta(edit, (suppressed ? std::string("Suppress ") : std::string("Unsuppress ")) +
                              feature->name());
    }
    // A PlaceholderFeature is deliberately never registered and never joins the
    // graph (ADR-009 D4), so NodeNotFound is its NORMAL answer, not a failure:
    // the feature is a real feature and its state was really set.
    if (!graphResult && graphResult.error == GraphError::NodeNotFound) return GraphResult{};
    return graphResult;
}

// NOT FIXED HERE, and named so it is not mistaken for fixed: M2's rule is that
// dirtiness propagates THROUGH a suppressed node and its dependents execute
// normally (`M2_SUP_002_DownstreamOfSuppressedExecutes`, whose own comment says
// this was safe only because "M2 nodes have no output contract" and that "real
// cached-output semantics arrive with CAD features in M3"). M3 through M8 never
// revisited it. On an M8 chain the rule is wrong -- a suppressed Pocket's
// consumer still runs, against whatever shape the suppressed pocket last
// retained. That is a semantics change with an ADR attached, and it belongs to
// M9.3 (M9_SPEC.md section 3.1), which owes an executed demonstration first.

// Keeps Feature::state() -- the manually-synchronized cache of ADR-M3-004 --
// from claiming Valid when the graph, which is the sole source of truth, says
// the feature is not current.
//
// Only a false Valid is corrected, and only ever downward to Dirty ("needs
// recompute"). Failed is never manufactured here: a feature is Failed solely
// because its own recompute() ran and reported failure, and that remains the
// only way to reach that state.
//
// Two concrete falsehoods this removes, both observable through the public API:
//   * a restored feature read Valid while holding no runtime shape at all --
//     ComputeState is persisted, KernelShape deliberately is not (ADR-M3-005),
//     so a reload always produced a feature claiming validity it could not have;
//   * after a Width/Height/Depth parameter edit the graph dirties the feature by
//     propagation, but nothing wrote through to the cache, so it kept reading
//     Valid while holding superseded geometry.
//
// Feature::markDirty() had no callers anywhere in src/ before this.
//
// SCOPE (ADR-M3-007): this applies ONLY to features that are graph nodes.
// Features are heterogeneous as of M3 -- BoxFeature is the first and so far
// only type that joins the graph (through wireBoxFeature), while a
// PlaceholderFeature is Body-owned and never registered. A feature outside the
// graph has no graph state to be authoritative over: its ComputeState is owned
// by whoever drives it, is persisted verbatim, and must not be rewritten here.
// Skipping it is the correct semantics, not merely crash avoidance -- though it
// is also that, since DependencyGraph::state() asserts on an unknown id
// (process abort in Debug, a bogus Failed in Release).
void PartDocument::syncFeatureStatesFromGraph() noexcept {
    // PARAMETERS FIRST (M11.2), and this loop closes a gap that only opened
    // when parameters gained edges to each other.
    //
    // ADR-011's bridge kept ParameterState in step with the graph for the ONE
    // shape that existed before: a parameter is dirtied by its own edit, and
    // that edit also sets ParameterState::Dirty. A parameter had no
    // prerequisites, so nothing else could make its node stale.
    //
    // `#Width / 2` breaks that. Editing Width dirties Height's graph node
    // through the ordinary machinery, while Height's own ParameterState stayed
    // Valid -- so a stale value reported itself as current, which is precisely
    // the staleness invariant ADR-M3-004 states for features. Four M11.2 tests
    // failed on it.
    //
    // Same rule as the feature loop below: only Valid is demoted, so a Failed
    // parameter is not quietly downgraded to merely Dirty.
    for (const std::unique_ptr<Parameter>& parameter : parameters_.items()) {
        // ONLY expression-driven parameters. A literal parameter is not
        // "stale" -- it is the number the user typed, and it is current by
        // definition. Its graph node starts Dirty at creation (it has never
        // been through a recompute), so a broader rule demoted every freshly
        // added parameter to Dirty and broke an accepted round-trip test that
        // had every right to expect Valid. ParameterState is a derived-currency
        // signal only when something derives it.
        if (parameter->expression().empty()) continue;
        if (parameter->state() != ParameterState::Valid) continue;
        if (!graph_.hasNode(parameter->id())) continue;
        if (graph_.state(parameter->id()) != ComputeState::Valid)
            parameter->markEvaluationDirty();
    }

    for (const std::unique_ptr<Body>& body : bodies_) {
        for (const std::unique_ptr<Feature>& feature : body->features()) {
            if (feature->state() != ComputeState::Valid) continue;
            if (!graph_.hasNode(feature->id())) continue; // not graph-scheduled
            if (graph_.state(feature->id()) != ComputeState::Valid) feature->markDirty();
        }
    }

    // Same rule, same moment, for the derived mass properties (ADR-M3-006):
    // once the graph says the mass node is no longer current, the numbers it
    // last produced stop being current too. Without this the two staleness
    // signals disagreed between an edit and the next recompute -- a feature
    // correctly read Dirty while massProperties().valid still read true.
    if (graph_.hasNode(massPropertiesNode_.id()) &&
        graph_.state(massPropertiesNode_.id()) != ComputeState::Valid) {
        massProperties_.valid = false;
    }
}

// Retention-with-staleness at the DATA level (ADR-M3-004, spec 2 DoD
// "downstream current results do not falsely succeed", spec 13, spec 19-C).
//
// MassPropertiesNode retains the last valid numbers on failure, mirroring
// BoxFeature::currentShape_. For a shape that is sufficient, because every
// consumer reaches it through a node whose ComputeState says whether it is
// current. massProperties() has no such guard: it hands out a plain struct, so
// its own `valid` flag is the ONLY staleness signal a direct reader ever sees,
// and it must be cleared whenever the node did not succeed this pass.
//
// Doing this here rather than only inside MassPropertiesNode::recompute() is
// what makes it correct: when an upstream BoxFeature fails, the graph blocks
// this node with BlockedByDependency and never invokes it at all, so no code
// inside the node can run to clear the flag. The engine's report is the one
// place that sees blocked and persisted-failure nodes alike (see
// RecomputeTypes.h on item ordering).
//
// A node absent from `items` was not touched this pass, so its previous
// currency stands unchanged.
void PartDocument::refreshMassPropertiesCurrency(const DocumentRecomputeReport& report) noexcept {
    for (const RecomputeItemReport& item : report.items) {
        if (item.id != massPropertiesNode_.id()) continue;
        if (item.status != RecomputeStatus::Success) massProperties_.valid = false;
        return;
    }
}

DocumentRecomputeReport PartDocument::recompute() {
    const DocumentRecomputeReport report = engine_.recompute();
    refreshMassPropertiesCurrency(report);
    syncFeatureStatesFromGraph();
    return report;
}

DocumentRecomputeReport PartDocument::recomputeFrom(ObjectId id) {
    const DocumentRecomputeReport report = engine_.recomputeFrom(id);
    refreshMassPropertiesCurrency(report);
    syncFeatureStatesFromGraph();
    return report;
}

bool PartDocument::removeObject(ObjectId id) {
    ObjectRegistry::ObjectRef* found = registry_.find(id);
    if (found == nullptr) return false;
    const ObjectRegistry::ObjectRef handle = *found; // copy before unregistering

    // --- Undo bookkeeping (M9.1), decided BEFORE anything is unhooked -------
    //
    // Three outcomes, and the middle one is the honest part:
    //   * a feature nothing consumes  -> recorded, and undo restores it;
    //   * a feature something CONSUMES -> the removal leaves the consumer with
    //     a dangling base and the consumer's chain edge is dropped with the
    //     node, so restoring the feature alone would NOT restore the document.
    //     M9.1 cannot replay it, so it CLEARS the history instead of keeping
    //     one that would silently do the wrong thing;
    //   * anything else (Body, Parameter, Sketch, Material) -> same reasoning,
    //     same clearing. Bodies take their features with them; parameters and
    //     sketches are referenced by features that would be left dangling.
    //
    // The clearing is observable -- undoDepth() drops to zero -- so a UI can
    // tell the user the history ended rather than offering an undo that lies.
    // Restoring these is M9.2/M9.3 work, named here rather than half-done.
    // A frame's removal is recordable, so deleting one does not end the history
    // (M10). What it does NOT do is cascade: a sketch supported by a removed
    // frame FAILS loudly and save refuses the dangling reference, which is the
    // accepted M4 precedent for deleting a sketch a Pad reads, applied
    // unchanged rather than quietly reversed here.
    if (std::holds_alternative<Connector*>(handle) && !applyingHistory_) {
        const Connector* connector = std::get<Connector*>(handle);
        ConnectorExistenceEdit edit;
        edit.connectorId = id;
        edit.name = connector->name();
        edit.role = static_cast<int>(connector->role());
        edit.frameId = connector->frameId();
        edit.owner = static_cast<int>(connector->owner());
        edit.addedByTheEdit = false;
        recordDelta(edit, "Delete " + connector->name());
    }
    if (std::holds_alternative<ReferenceFrame*>(handle) && !applyingHistory_) {
        const ReferenceFrame* frame = std::get<ReferenceFrame*>(handle);
        FrameExistenceEdit edit;
        edit.frameId = id;
        edit.name = frame->name();
        edit.parentFrameId = frame->parentFrameId();
        edit.localTransform = frame->localTransform();
        edit.addedByTheEdit = false;
        recordDelta(edit, "Delete " + frame->name());
    }
    if (std::holds_alternative<Parameter*>(handle) && !applyingHistory_) {
        const Parameter* parameter = std::get<Parameter*>(handle);
        ParameterExistenceEdit edit;
        edit.parameterId = id;
        edit.name = parameter->name();
        edit.value = parameter->value();
        edit.unit = static_cast<int>(parameter->unit());
        edit.expression = parameter->expression();
        edit.addedByTheEdit = false;
        recordDelta(edit, "Delete " + parameter->name());
    }
    const Feature* removedFeature = nullptr;
    const Body* owningBody = nullptr;
    std::size_t removedIndex = 0;
    for (const std::unique_ptr<Body>& body : bodies_) {
        for (std::size_t i = 0; i < body->features().size(); ++i) {
            if (body->features()[i]->id() != id) continue;
            removedFeature = body->features()[i].get();
            owningBody = body.get();
            removedIndex = i;
        }
    }
    bool consumedByAnother = false;
    if (removedFeature != nullptr) {
        for (const std::unique_ptr<Body>& body : bodies_)
            for (const std::unique_ptr<Feature>& feature : body->features())
                if (const auto* solid = dynamic_cast<const ISolidFeature*>(feature.get()))
                    for (const ObjectId eaten : solid->consumedSolidIds())
                        if (eaten == id) consumedByAnother = true;
    }
    if (removedFeature != nullptr && !consumedByAnother) {
        FeatureExistenceEdit edit;
        edit.bodyId = owningBody->id();
        edit.index = removedIndex;
        edit.snapshot = SnapshotFeature(*removedFeature);
        edit.addedByTheEdit = false;
        recordDelta(edit, "Delete " + removedFeature->name());
    } else if (!applyingHistory_ && !std::holds_alternative<Parameter*>(handle) &&
               !std::holds_alternative<ReferenceFrame*>(handle) &&
               !std::holds_alternative<Connector*>(handle)) {
        clearHistory(removedFeature != nullptr ? "a consumed feature was removed"
                                               : "a non-feature object was removed");
    }

    // A Parameter bound by a sketch constraint is REFUSED, not cascaded
    // (ADR-M5-009). The asymmetry with entity deletion is deliberate: a
    // constraint's entity is private to its sketch and is gone for good, but a
    // Parameter is a named, shared, document-level object the user can see and
    // re-point. Silently deleting their dimensional constraints as a side
    // effect of deleting a parameter destroys more than was asked for.
    //
    // Checked BEFORE anything is unhooked, so a refusal leaves the document
    // byte-for-byte unchanged rather than half-removed.
    if (std::holds_alternative<Parameter*>(handle) &&
        !constraintsBindingParameter(id).empty())
        return false;

    // The same rule for a Parameter another parameter's EXPRESSION reads
    // (M11.2). Identical reasoning to ADR-M5-009, applied to the second kind of
    // reference a named parameter can attract: silently deleting someone's
    // formulas as a side effect of deleting a parameter destroys more than was
    // asked for -- and unlike a constraint, the formula would not even be
    // visibly gone, it would just stop resolving.
    //
    // Also checked BEFORE anything is unhooked, so a refusal leaves the
    // document byte-for-byte unchanged.
    if (std::holds_alternative<Parameter*>(handle) &&
        !parametersReferencingParameter(id).empty())
        return false;

    // NOT extended to Sketches, deliberately.
    //
    // Independent review recommended refusing to delete a Sketch a Pad reads,
    // by analogy with the Parameter rule above. That would OVERTURN an accepted
    // M4 contract: M4's own review took this exact case (MAJOR2/MAJOR3) and
    // settled on "deletion is allowed, the Pad fails LOUDLY, and save refuses
    // to write a document with a dangling Pad" -- so a broken document can
    // never overwrite a good file, and the user recovers by deleting the Pad.
    // Three accepted tests encode that.
    //
    // The reviewer's underlying complaint is real: until the Pad is removed,
    // every save fails. But that is the accepted design working, not a defect
    // introduced by M5, and reversing a reviewed milestone decision is the
    // owner's call, not a side effect of fixing something else. Recorded as an
    // open question in the M5 review response instead.

    // Order matters (spec 12): graph first (edges cleaned in both directions,
    // former dependents dirtied per ADR-007), then registry, then owner.
    graph_.removeNode(id); // NodeNotFound is fine -- bodies have no graph node
    registry_.unregisterObject(id);

    // Removing the mass-properties node also removes the only thing that could
    // keep its result current. Leaving massProperties_.valid true meant the
    // status bar went on reporting a stale volume as CURRENT through every
    // later edit -- the currency invariant of ADR-M3-004, broken silently,
    // because syncFeatureStatesFromGraph skips a node the graph no longer has.
    if (id == massPropertiesNode_.id()) massProperties_ = MassProperties{};

    if (std::holds_alternative<Parameter*>(handle)) {
        parameters_.remove(id);
    } else if (std::holds_alternative<Body*>(handle)) {
        for (auto it = bodies_.begin(); it != bodies_.end(); ++it) {
            if ((*it)->id() != id) continue;

            // Unhook every feature this Body OWNS before destroying it.
            // Without this the features stayed registered and graph-scheduled
            // while their memory was freed, and the next recompute() called
            // recompute() on a destroyed PadFeature -- a use-after-free that
            // savePartDocument happily preceded, so the crash landed later and
            // somewhere else. ObjectRegistry's own header states the opposite
            // invariant ("removeObject unhooks graph and registry BEFORE the
            // owner erases"); this is the branch that did not honour it.
            for (const std::unique_ptr<Feature>& feature : (*it)->features()) {
                const ObjectId featureId = feature->id();
                // The mass-properties node holds a feature id by value, so a
                // destroyed source must be detached rather than left dangling.
                if (massPropertiesNode_.boxFeatureId() == featureId) {
                    massPropertiesNode_.setSource(kInvalidObjectId,
                                                  massPropertiesNode_.materialId());
                    graph_.removeNode(massPropertiesNode_.id());
                    massProperties_ = MassProperties{}; // no source -> nothing current
                }
                graph_.removeNode(featureId);
                registry_.unregisterObject(featureId);
            }

            bodies_.erase(it);
            break;
        }
        syncFeatureStatesFromGraph();
    } else if (std::holds_alternative<Sketch*>(handle)) {
        // Without this the sketch survived removal: still in sketches(), still
        // resolvable, still serialized and fully resurrected on reload, while
        // its graph node was gone so markSketchDirty could never recover it.
        // Same defect class as ADR-M3-008's Body-owned feature, one type later.
        //
        // Dependents are already dirtied by graph_.removeNode(id) above
        // (ADR-007: removing a node dirties its former dependents), so a Pad
        // reading this sketch fails loudly on its next recompute rather than
        // continuing to report a solid built from geometry that no longer
        // exists. An explicit markDirty here would be dead code -- the node is
        // gone by this point and it would return false.
        for (auto it = sketches_.begin(); it != sketches_.end(); ++it) {
            if ((*it)->id() != id) continue;
            sketches_.erase(it);
            break;
        }
        syncFeatureStatesFromGraph();
    } else if (std::holds_alternative<Material*>(handle)) {
        if (material_ && material_->id() == id) {
            if (massPropertiesNode_.materialId() == id)
                massPropertiesNode_.setSource(massPropertiesNode_.boxFeatureId(),
                                              kInvalidObjectId);
            material_.reset();
            massProperties_.valid = false; // mass can no longer be current

            // Clearing the owner is not enough: every FEATURE still holding the
            // removed id would keep writing it, so the file would save cleanly
            // and its own loader would reject it forever. Removal has to reach
            // referrers too (ADR-M4-009, extended after review).
            //
            // Found by independent review as a defect INTRODUCED by the fix that
            // added this branch -- the second time in this project that repairing
            // a Major created a new one.
            for (const std::unique_ptr<Body>& body : bodies_) {
                for (const std::unique_ptr<Feature>& feature : body->features()) {
                    auto* referencing = dynamic_cast<IMaterialReferencing*>(feature.get());
                    if (referencing == nullptr) continue;
                    if (referencing->materialId() != id) continue;
                    referencing->clearMaterialReference();
                }
            }
        }
    } else if (std::holds_alternative<ReferenceFrame*>(handle)) {
        for (auto it = frames_.begin(); it != frames_.end(); ++it)
            if ((*it)->id() == id) {
                frames_.erase(it);
                break;
            }
    } else if (std::holds_alternative<Connector*>(handle)) {
        for (auto it = connectors_.begin(); it != connectors_.end(); ++it)
            if ((*it)->id() == id) {
                connectors_.erase(it);
                break;
            }
    } else if (std::holds_alternative<IRecomputable*>(handle)) {
        // An IRecomputable is normally externally owned, so there is no owner
        // step -- but since M3 a BoxFeature registers through this same variant
        // alternative while being owned by its Body. Without the owner step
        // below, removeObject() would report success while leaving the feature
        // alive in Body::features(): still serialized, still restored on load,
        // and still the massPropertiesNode_'s source, which then fails every
        // subsequent recompute() with "no box feature configured" forever.
        //
        // Detaching the mass-properties node comes FIRST: it holds the id by
        // value, so leaving it set would point it at a destroyed feature.
        if (massPropertiesNode_.boxFeatureId() == id) {
            massPropertiesNode_.setSource(kInvalidObjectId, massPropertiesNode_.materialId());
            graph_.removeNode(massPropertiesNode_.id());
            massProperties_ = MassProperties{}; // no source -> nothing current
        }
        // Erase the object the registry actually resolved, not "the first
        // feature carrying this id" (round 4, R1R4-C1). The registry stores a
        // feature under the IRecomputable alternative; Feature is a separate
        // base, so the cross-cast is dynamic.
        const auto* asFeature = dynamic_cast<const Feature*>(std::get<IRecomputable*>(handle));
        const Body* ownerOfRemoved = nullptr;
        for (const std::unique_ptr<Body>& body : bodies_)
            if (body->removeFeature(asFeature)) {
                ownerOfRemoved = body.get();
                break;
            }
        // The chain lost a link, so the mass source follows the NEW tail
        // (ADR-M8-003). M8 recorded "removeObject does not re-point mass at the
        // new tail" as a known limitation and left it to M9; gate C requires
        // it, so this is where it is paid.
        if (ownerOfRemoved != nullptr) rewireMassPropertiesToTail(*ownerOfRemoved);
    }
    return true;
}

} // namespace paramcad
