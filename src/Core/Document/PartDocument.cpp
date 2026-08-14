#include "Core/Document/PartDocument.h"
#include "Core/Feature/BoxFeature.h"
#include "Core/Feature/IMaterialReferencing.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/PocketFeature.h"
#include "Core/Recompute/IRecomputable.h"
#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace paramcad {

PartDocument::PartDocument(std::string name)
    : CadDocument(std::move(name)) {
    addFrame("Origin");
    // MassPropertiesNode is auto-created fresh and auto-registered in the
    // registry (never persisted, ADR-M3-005 -- exactly like the Origin
    // frame), so it is always resolvable. It only JOINS THE GRAPH once
    // wireBoxFeature (via addBoxFeature/restoreBoxFeature) actually gives it
    // a source -- a document with no BoxFeature must not carry a permanently
    // Dirty, permanently failing, edge-less recompute node.
    registry_.registerObject(massPropertiesNode_.id(), &massPropertiesNode_);
}

PartDocument::PartDocument(ObjectId id, std::string name)
    : CadDocument(id, std::move(name)) {
    addFrame("Origin");
    registry_.registerObject(massPropertiesNode_.id(), &massPropertiesNode_);
}

Parameter& PartDocument::addParameter(std::string name, double value, UnitType unit) {
    Parameter& parameter = parameters_.add(std::move(name), value, unit);
    registry_.registerObject(parameter.id(), &parameter);
    graph_.addNode(parameter.id());
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
    if (registry_.contains(id))
        throw std::runtime_error("restoreParameter: id " + std::to_string(id) +
                                 " is already registered in this document");

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
    const ObjectRegistry::ObjectRef* ref = registry_.find(id);
    if (ref == nullptr) return false;
    auto* const* parameter = std::get_if<Parameter*>(ref);
    if (parameter == nullptr) return false;
    (*parameter)->setValue(value); // ParameterState -> Dirty
    graph_.markDirty(id);          // propagate to dependents
    syncFeatureStatesFromGraph();
    return true;
}

Body& PartDocument::addBody(std::string name) {
    auto item = std::make_unique<Body>(std::move(name));
    auto& ref = *item;
    bodies_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    return ref;
}

Body& PartDocument::restoreBody(ObjectId id, std::string name) {
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

ReferenceFrame& PartDocument::addFrame(std::string name, ObjectId parentFrameId) {
    auto item = std::make_unique<ReferenceFrame>(std::move(name), parentFrameId);
    auto& ref = *item;
    frames_.push_back(std::move(item));
    return ref;
}

Connector& PartDocument::addConnector(std::string name, ConnectorRole role, ObjectId frameId) {
    auto item = std::make_unique<Connector>(std::move(name), role, frameId);
    auto& ref = *item;
    connectors_.push_back(std::move(item));
    return ref;
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
    if (registry_.contains(id))
        throw std::runtime_error("restoreMaterial: id " + std::to_string(id) +
                                 " is already registered in this document");

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
    if (!sketch->removeConstraint(constraintId)) return false;

    // One reconciler, not a per-path rule: it drops the edge only when nothing
    // else on this sketch still binds that Parameter, because two dimensions
    // legitimately share one.
    (void)parameterId;
    reconcileSketchParameterEdges(sketchId);

    graph_.markDirty(sketchId);
    syncFeatureStatesFromGraph();
    return true;
}

bool PartDocument::removeSketchEntity(ObjectId sketchId, SketchEntityId entityId) {
    Sketch* sketch = findSketchForEdit(sketchId);
    if (sketch == nullptr) return false;

    const Sketch::EntityRemoval removal = sketch->removeEntityCascading(entityId);
    if (!removal.removed) return false;

    // The released list is not a removal list -- a surviving constraint may
    // still bind the same Parameter -- so the edges are reconciled against the
    // constraint set that actually remains.
    (void)removal;
    reconcileSketchParameterEdges(sketchId);

    graph_.markDirty(sketchId);
    syncFeatureStatesFromGraph();
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
    std::vector<ObjectId> ids;
    if (sketchId == kInvalidObjectId) return ids;
    for (const std::unique_ptr<Body>& body : bodies_)
        for (const std::unique_ptr<Feature>& feature : body->features())
            if (const auto* pad = dynamic_cast<const PadFeature*>(feature.get()))
                if (pad->sketchId() == sketchId) ids.push_back(pad->id());
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
    return feature;
}

PadFeature& PartDocument::restorePadFeature(Body& body, ObjectId id, std::string name,
                                            ComputeState state, ObjectId sketchId,
                                            ObjectId lengthParameterId, ObjectId materialId) {
    if (registry_.contains(id))
        throw std::runtime_error("restorePadFeature: id " + std::to_string(id) +
                                 " is already registered in this document");
    PadFeature& feature = body.addFeature<PadFeature>(id, std::move(name), state, sketchId,
                                                      lengthParameterId, materialId);
    wirePadFeature(feature, sketchId, lengthParameterId, materialId);
    return feature;
}


void PartDocument::wirePocketFeature(PocketFeature& feature, ObjectId baseFeatureId,
                                     ObjectId sketchId, ObjectId depthParameterId,
                                     ObjectId materialId) {
    addRecomputableNode(feature); // registry + graph node (IRecomputable*)
    // THE chain edge (ADR-M8-001): editing anything that rebuilds the base
    // dirties the pocket through the ordinary M2 machinery. Without it, a
    // Width edit would rebuild the pad and leave the pocket cutting yesterday's
    // solid -- current-looking, analytically wrong.
    addDependency(feature.id(), baseFeatureId);     // Base   -> Pocket
    addDependency(feature.id(), sketchId);          // Sketch -> Pocket
    addDependency(feature.id(), depthParameterId);  // Depth  -> Pocket
    // Physics follows the chain TAIL (ADR-M8-003): the pocketed result is the
    // part; the pad alone is now an intermediate value.
    rewireMassPropertiesSource(feature.id(), materialId);
}

PocketFeature& PartDocument::addPocketFeature(Body& body, std::string name,
                                              ObjectId baseFeatureId, ObjectId sketchId,
                                              ObjectId depthParameterId) {
    const ObjectId materialId = material_ ? material_->id() : kInvalidObjectId;
    PocketFeature& feature = body.addFeature<PocketFeature>(
        std::move(name), baseFeatureId, sketchId, depthParameterId, materialId);
    wirePocketFeature(feature, baseFeatureId, sketchId, depthParameterId, materialId);
    return feature;
}

PocketFeature& PartDocument::restorePocketFeature(Body& body, ObjectId id, std::string name,
                                                  ComputeState state, ObjectId baseFeatureId,
                                                  ObjectId sketchId, ObjectId depthParameterId,
                                                  ObjectId materialId) {
    // Same duplicate-id guard as every restored type (ADR-M5-018, and the M6
    // review that found the types the fix skipped).
    if (registry_.contains(id))
        throw std::runtime_error("restorePocketFeature: id " + std::to_string(id) +
                                 " is already registered in this document");
    PocketFeature& feature = body.addFeature<PocketFeature>(
        id, std::move(name), state, baseFeatureId, sketchId, depthParameterId, materialId);
    wirePocketFeature(feature, baseFeatureId, sketchId, depthParameterId, materialId);
    return feature;
}

BoxFeature& PartDocument::addBoxFeature(Body& body, std::string name, ObjectId widthParameterId,
                                        ObjectId heightParameterId, ObjectId depthParameterId) {
    const ObjectId materialId = material_ ? material_->id() : kInvalidObjectId;
    BoxFeature& feature = body.addFeature<BoxFeature>(std::move(name), widthParameterId,
                                                       heightParameterId, depthParameterId,
                                                       materialId);
    wireBoxFeature(feature, widthParameterId, heightParameterId, depthParameterId, materialId);
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
    if (registry_.contains(id))
        throw std::runtime_error("restoreBoxFeature: id " + std::to_string(id) +
                                 " is already registered in this document");
    BoxFeature& feature = body.addFeature<BoxFeature>(id, std::move(name), state,
                                                       widthParameterId, heightParameterId,
                                                       depthParameterId, materialId);
    wireBoxFeature(feature, widthParameterId, heightParameterId, depthParameterId, materialId);
    return feature;
}

GraphResult PartDocument::addRecomputableNode(IRecomputable& object) {
    if (!registry_.registerObject(object.id(), &object)) {
        return {object.id() == kInvalidObjectId ? GraphError::NodeNotFound
                                                : GraphError::NodeAlreadyExists};
    }
    const GraphResult result = graph_.addNode(object.id());
    if (!result) registry_.unregisterObject(object.id()); // keep the two in sync
    return result;
}

GraphResult PartDocument::addDependency(ObjectId dependent, ObjectId prerequisite) {
    return graph_.addDependency(dependent, prerequisite);
}

GraphResult PartDocument::removeDependency(ObjectId dependent, ObjectId prerequisite) {
    return graph_.removeDependency(dependent, prerequisite);
}

bool PartDocument::markDirty(ObjectId id) {
    if (!graph_.markDirty(id)) return false;
    if (const ObjectRegistry::ObjectRef* ref = registry_.find(id))
        if (auto* const* parameter = std::get_if<Parameter*>(ref))
            (*parameter)->markEvaluationDirty(); // ADR-011 bridge
    syncFeatureStatesFromGraph();
    return true;
}

GraphResult PartDocument::setSuppressed(ObjectId id, bool suppressed) {
    return graph_.setSuppressed(id, suppressed);
}

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
    const ObjectRegistry::ObjectRef* found = registry_.find(id);
    if (found == nullptr) return false;
    const ObjectRegistry::ObjectRef handle = *found; // copy before unregistering

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
        for (const std::unique_ptr<Body>& body : bodies_)
            if (body->removeFeature(id)) break;
    }
    return true;
}

} // namespace paramcad
