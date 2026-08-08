#include "Core/Document/PartDocument.h"
#include "Core/Feature/BoxFeature.h"
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

bool PartDocument::setMaterialDensity(double densityKgPerM3) {
    if (!material_) return false;
    material_->setDensity(densityKgPerM3);
    graph_.markDirty(material_->id()); // propagate to MassPropertiesNode
    return true;
}

void PartDocument::wireBoxFeature(BoxFeature& feature, ObjectId widthParameterId,
                                  ObjectId heightParameterId, ObjectId depthParameterId,
                                  ObjectId materialId) {
    addRecomputableNode(feature); // registry + graph node (IRecomputable*)
    addDependency(feature.id(), widthParameterId);
    addDependency(feature.id(), heightParameterId);
    addDependency(feature.id(), depthParameterId);

    // MassPropertiesNode joins the graph on first use (see the constructors'
    // comment): a document that never adds a BoxFeature must not carry a
    // permanently Dirty, permanently failing, edge-less recompute node.
    if (!graph_.hasNode(massPropertiesNode_.id())) graph_.addNode(massPropertiesNode_.id());

    // Re-wiring the singleton MassPropertiesNode to a (possibly new) box
    // source: detach any previous source's edges first so the graph never
    // accumulates stale prerequisites from an earlier box (M3 scoping note,
    // ADR-M3-005).
    if (massPropertiesNode_.boxFeatureId() != kInvalidObjectId)
        removeDependency(massPropertiesNode_.id(), massPropertiesNode_.boxFeatureId());
    if (massPropertiesNode_.materialId() != kInvalidObjectId)
        removeDependency(massPropertiesNode_.id(), massPropertiesNode_.materialId());

    massPropertiesNode_.setSource(feature.id(), materialId);
    addDependency(massPropertiesNode_.id(), feature.id()); // BoxFeature -> MassPropertiesNode
    if (materialId != kInvalidObjectId)
        addDependency(massPropertiesNode_.id(), materialId); // Material -> MassPropertiesNode
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
    return true;
}

GraphResult PartDocument::setSuppressed(ObjectId id, bool suppressed) {
    return graph_.setSuppressed(id, suppressed);
}

DocumentRecomputeReport PartDocument::recompute() {
    return engine_.recompute();
}

DocumentRecomputeReport PartDocument::recomputeFrom(ObjectId id) {
    return engine_.recomputeFrom(id);
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
    }
    // Feature*: not registered in M2 (features are Body-owned and join the
    // document graph in M3). IRecomputable*: externally owned, no owner step.
    return true;
}

} // namespace paramcad
