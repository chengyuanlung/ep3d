#include "Core/Document/PartDocument.h"
#include "Core/Feature/BoxFeature.h"
#include "Core/Feature/IMaterialReferencing.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Recompute/IRecomputable.h"
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
    Parameter& parameter =
        parameters_.restore(id, std::move(name), value, unit, std::move(expression), state);
    registry_.registerObject(parameter.id(), &parameter);
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
    registry_.registerObject(ref.id(), &ref);
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

Material& PartDocument::addMaterial(std::string name, double densityKgPerM3) {
    auto item = std::make_shared<Material>(std::move(name), densityKgPerM3);
    Material& ref = *item;
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
    auto item = std::make_shared<Material>(id, std::move(name), densityKgPerM3, elasticModulusPa,
                                           poissonRatio, yieldStrengthPa, contact);
    Material& ref = *item;
    material_ = std::move(item);
    registry_.registerObject(ref.id(), &ref);
    graph_.addNode(ref.id()); // starts Dirty; graph states are not persisted
    return ref;
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
    registry_.registerObject(ref.id(), &ref);
    graph_.addNode(ref.id()); // dirty source, exactly like Parameter/Material
    return ref;
}

Sketch& PartDocument::restoreSketch(ObjectId id, std::string name, SketchFrame frame) {
    auto item = std::make_unique<Sketch>(id, std::move(name), frame);
    Sketch& ref = *item;
    sketches_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
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

bool PartDocument::editSketch(ObjectId sketchId, const std::function<void(Sketch&)>& edit) {
    Sketch* sketch = findSketchForEdit(sketchId);
    if (sketch == nullptr || !edit) return false;
    edit(*sketch);
    // Dirtying is not optional and not the caller's responsibility: an edited
    // sketch whose dependents were never marked stale would keep reporting a
    // solid built from geometry that no longer exists.
    graph_.markDirty(sketchId);
    syncFeatureStatesFromGraph();
    return true;
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
    PadFeature& feature = body.addFeature<PadFeature>(id, std::move(name), state, sketchId,
                                                      lengthParameterId, materialId);
    wirePadFeature(feature, sketchId, lengthParameterId, materialId);
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

    // Order matters (spec 12): graph first (edges cleaned in both directions,
    // former dependents dirtied per ADR-007), then registry, then owner.
    graph_.removeNode(id); // NodeNotFound is fine -- bodies have no graph node
    registry_.unregisterObject(id);

    if (std::holds_alternative<Parameter*>(handle)) {
        parameters_.remove(id);
    } else if (std::holds_alternative<Body*>(handle)) {
        for (auto it = bodies_.begin(); it != bodies_.end(); ++it) {
            if ((*it)->id() == id) {
                bodies_.erase(it);
                break;
            }
        }
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
