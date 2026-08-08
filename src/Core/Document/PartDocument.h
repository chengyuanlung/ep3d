#pragma once

#include "Core/Body/Body.h"
#include "Core/Connector/Connector.h"
#include "Core/Dependency/DependencyGraph.h"
#include "Core/Document/CadDocument.h"
#include "Core/Document/ObjectRegistry.h"
#include "Core/Material/Material.h"
#include "Core/Parameter/ParameterManager.h"
#include "Core/Physics/MassProperties.h"
#include "Core/Recompute/DocumentRecomputeEngine.h"
#include "Core/Recompute/RecomputeTypes.h"
#include "Core/Reference/ReferenceFrame.h"
#include <memory>
#include <vector>

namespace paramcad {

class IRecomputable;

// DEPENDENCY DIRECTION (single rule, ADR-007/ADR-012): an edge points
// prerequisite -> dependent; "A -> B" means B depends on A and dirtiness
// flows from A downstream to B. addDependency(dependent, prerequisite) reads
// "dependent consumes prerequisite" -- the facade mirrors the DependencyGraph
// signature exactly so the project has one parameter order everywhere.
//
// The document is the single registration path (spec 13): addParameter /
// restoreParameter / addBody / restoreBody / addRecomputableNode keep owner,
// ObjectRegistry, and DependencyGraph consistent; removeObject unhooks
// graph -> registry -> owner in that order so no dangling reference is
// reachable through a public path. Registry/graph accessors are const-only;
// all mutation goes through the facade.
class PartDocument final : public CadDocument {
public:
    explicit PartDocument(std::string name);
    // Restore constructor (deserialization): keeps the persisted document id.
    // Frames are not serialized, so the Origin frame is auto-created with a
    // fresh id, exactly as in the fresh constructor.
    PartDocument(ObjectId id, std::string name);

    // engine_ is self-referencing (holds PartDocument&); an implicit copy
    // would bind the copy's engine_ to the ORIGINAL document, and a move
    // would leave the moved-from document's engine_ dangling. Not currently
    // reachable (nothing copies/moves a PartDocument today), disabled as
    // cheap insurance.
    PartDocument(const PartDocument&) = delete;
    PartDocument& operator=(const PartDocument&) = delete;
    PartDocument(PartDocument&&) = delete;
    PartDocument& operator=(PartDocument&&) = delete;

    DocumentType type() const noexcept override { return DocumentType::Part; }

    // --- Parameters (dirty sources, ADR-011; no graph execution body) ------
    Parameter& addParameter(std::string name, double value, UnitType unit);
    Parameter& restoreParameter(ObjectId id, std::string name, double value, UnitType unit,
                                std::string expression, ParameterState state);
    // Sets the value (ParameterState -> Dirty) AND marks the graph node dirty
    // (propagates to dependents). False if the id is unknown or not a
    // Parameter.
    bool setParameterValue(ObjectId id, double value);
    // BREAKING vs M1: const-only. Use addParameter/setParameterValue/
    // removeObject for mutation (single registration path).
    const ParameterManager& parameters() const noexcept { return parameters_; }

    // --- Bodies (registered; NO graph node in M2 -- nothing recomputes them)
    Body& addBody(std::string name);
    // Restore path (deserialization): adds a body that keeps its persisted id.
    Body& restoreBody(ObjectId id, std::string name);
    const std::vector<std::unique_ptr<Body>>& bodies() const noexcept { return bodies_; }

    ReferenceFrame& addFrame(std::string name, ObjectId parentFrameId = kInvalidObjectId);
    const std::vector<std::unique_ptr<ReferenceFrame>>& frames() const noexcept { return frames_; }
    Connector& addConnector(std::string name, ConnectorRole role, ObjectId frameId);

    void setMaterial(std::shared_ptr<Material> material) { material_ = std::move(material); }
    const std::shared_ptr<Material>& material() const noexcept { return material_; }

    MassProperties& massProperties() noexcept { return massProperties_; }
    const MassProperties& massProperties() const noexcept { return massProperties_; }

    // --- Recompute infrastructure facade -----------------------------------
    // Registers an externally owned recomputable (e.g. a test stub) and gives
    // it a graph node. The caller guarantees the object outlives its
    // registration (remove with removeObject).
    GraphResult addRecomputableNode(IRecomputable& object);
    GraphResult addDependency(ObjectId dependent, ObjectId prerequisite);
    GraphResult removeDependency(ObjectId dependent, ObjectId prerequisite);
    // Graph markDirty plus the ADR-011 bridge: a Parameter node also gets
    // ParameterState::Dirty. False if the id has no graph node.
    bool markDirty(ObjectId id);
    GraphResult setSuppressed(ObjectId id, bool suppressed);
    DocumentRecomputeReport recompute();
    // markDirty(id) + full dirty-set recompute (see DocumentRecomputeEngine).
    DocumentRecomputeReport recomputeFrom(ObjectId id);

    // Removes a registered object everywhere, in this order: graph node
    // (edges cleaned, former dependents dirtied per ADR-007) -> registry ->
    // owning container (Parameter/Body; externally owned IRecomputables have
    // no owner step here). False if the id is not registered.
    bool removeObject(ObjectId id);

    // Const-only access; mutation goes through the facade above.
    const ObjectRegistry& objectRegistry() const noexcept { return registry_; }
    const DependencyGraph& dependencyGraph() const noexcept { return graph_; }

private:
    friend class DocumentRecomputeEngine; // engine drives graph_/registry_

    ParameterManager parameters_;
    std::vector<std::unique_ptr<Body>> bodies_;
    std::vector<std::unique_ptr<ReferenceFrame>> frames_;
    std::vector<std::unique_ptr<Connector>> connectors_;
    std::shared_ptr<Material> material_;
    MassProperties massProperties_;
    ObjectRegistry registry_;
    DependencyGraph graph_;
    DocumentRecomputeEngine engine_{*this};
};

} // namespace paramcad
