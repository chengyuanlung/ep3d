#pragma once

#include "Core/Body/Body.h"
#include "Core/Connector/Connector.h"
#include "Core/Dependency/DependencyGraph.h"
#include "Core/Document/CadDocument.h"
#include "Core/Document/ObjectRegistry.h"
#include "Core/Material/Material.h"
#include "Core/Parameter/ParameterManager.h"
#include "Core/Physics/MassProperties.h"
#include "Core/Physics/MassPropertiesNode.h"
#include "Core/Recompute/DocumentRecomputeEngine.h"
#include "Core/Recompute/RecomputeTypes.h"
#include "Core/Reference/ReferenceFrame.h"
#include "Core/Sketch/Sketch.h"
#include <functional>
#include <memory>
#include <vector>

namespace paramcad {

class IRecomputable;
class IGeometryKernel;
class BoxFeature;
class PadFeature;

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

    // --- Material (dirty source, ADR-M3-005; mirrors Parameter's pattern) --
    Material& addMaterial(std::string name, double densityKgPerM3);
    // Assigns the document's current Material to every feature that can hold
    // one, and rewires the mass-properties source. Without this, adding a
    // Material after a feature already exists left the feature unassigned and
    // the part weighing nothing -- self-consistent, round-trippable, and
    // visibly wrong to a user (raised as a Minor by independent review).
    // False if the document has no Material.
    bool assignMaterialToFeatures();

    Material& restoreMaterial(ObjectId id, std::string name, double densityKgPerM3,
                              double elasticModulusPa, double poissonRatio,
                              double yieldStrengthPa, ContactProperties contact);
    // Sets density (no validation here -- MassPropertiesNode::recompute
    // validates finite/non-negative, ADR-M3-005 density policy) AND marks the
    // graph node dirty, mirroring setParameterValue exactly. False if no
    // material is assigned.
    bool setMaterialDensity(double densityKgPerM3);
    const std::shared_ptr<Material>& material() const noexcept { return material_; }

    // NOTE: the non-const overload lets any caller overwrite derived state,
    // which sits awkwardly with "mutation goes through the facade" below.
    // MassPropertiesNode is its only legitimate writer. Left public
    // deliberately: every read through a non-const PartDocument selects this
    // overload too, so restricting it churns ~20 unrelated call sites for no
    // behavioural gain. Candidate M4 cleanup alongside the ADR-M3-004
    // Feature/IRecomputable collapse.
    MassProperties& massProperties() noexcept { return massProperties_; }
    const MassProperties& massProperties() const noexcept { return massProperties_; }

    // --- Box feature (ADR-M3-005; single registration path, spec 13) -------
    // Creates a BoxFeature in body, registers it as a graph-recomputable node
    // (IRecomputable*), wires Width/Height/Depth prerequisite edges, and
    // (re)wires the document's singleton MassPropertiesNode to this box (and
    // to the currently assigned Material, if any) -- the required graph shape
    // from spec 11. Re-wiring detaches any previous box/material source's
    // edges first so the graph never accumulates stale prerequisites.
    BoxFeature& addBoxFeature(Body& body, std::string name, ObjectId widthParameterId,
                              ObjectId heightParameterId, ObjectId depthParameterId);
    // Restore path (deserialization): same wiring, keeps the persisted
    // id/state and the persisted materialId (ADR-M3-005 Option B: this edge
    // is always re-derived from the semantic id field, never replayed from
    // the generic "dependencies" array).
    BoxFeature& restoreBoxFeature(Body& body, ObjectId id, std::string name, ComputeState state,
                                  ObjectId widthParameterId, ObjectId heightParameterId,
                                  ObjectId depthParameterId, ObjectId materialId);


    // --- Sketch (M4, ADR-M4-001/002/005) -----------------------------------
    // Creates a Sketch, registers it, and adds a graph node. A Sketch is a
    // DIRTY SOURCE like Parameter and Material (ADR-011's pattern reused
    // unchanged), not an IRecomputable: it has no derived state of its own.
    // Editing its geometry and calling markDirty propagates to dependent Pads.
    Sketch& addSketch(std::string name, SketchFrame frame = SketchFrame::WorldXY());
    // Restore path (deserialization): keeps the persisted id and frame.
    Sketch& restoreSketch(ObjectId id, std::string name, SketchFrame frame);
    // Const-only reads. A non-const Sketch& would let any caller edit geometry
    // without the graph learning the dependent Pads are stale -- exactly the
    // hazard M2 removed for Parameters by making parameters() const-only
    // (ADR-011). Editing goes through editSketch() below.
    const Sketch* findSketch(ObjectId id) const noexcept;

    // Returns const POINTERS, not the owning vector. `const vector<unique_ptr<
    // Sketch>>&` looks read-only and is not: constness stops at the unique_ptr,
    // so *doc.sketches().front() yields a mutable Sketch& from a const document
    // and geometry can be edited with nothing dirtied. Independent review
    // reproduced exactly that. The extra vector is a handful of pointers built
    // at serialization and inspection time, which is the right price for an
    // accessor that means what it says.
    std::vector<const Sketch*> sketches() const;

    // THE mutation path for sketch geometry. Applies `edit` to the sketch, then
    // marks it dirty so dependent Pads recompute. False if the id is not a
    // sketch in this document; in that case nothing is edited and nothing is
    // dirtied.
    //
    // Taking a callback rather than handing out a mutable reference is what
    // makes "edited" and "dirtied" inseparable: there is no way to do the first
    // without the second, which is the property a bare accessor cannot offer.
    bool editSketch(ObjectId sketchId, const std::function<void(Sketch&)>& edit);

    // Marks a sketch dirty without editing it -- for callers that mutated
    // through editSketch's reference and need to re-dirty, or that changed
    // something the document cannot see. Prefer editSketch.
    bool markSketchDirty(ObjectId sketchId);

    // --- Pad feature (M4, spec 12) -----------------------------------------
    // Creates a PadFeature in body, registers it as a graph node, wires the
    // Sketch and Length prerequisite edges, and (re)wires the document's
    // MassPropertiesNode to this pad -- giving exactly the graph spec 12
    // requires: Sketch -> Pad -> Mass, Length -> Pad, Material -> Mass.
    PadFeature& addPadFeature(Body& body, std::string name, ObjectId sketchId,
                              ObjectId lengthParameterId);
    PadFeature& restorePadFeature(Body& body, ObjectId id, std::string name, ComputeState state,
                                  ObjectId sketchId, ObjectId lengthParameterId,
                                  ObjectId materialId);

    // Non-owning; the caller keeps the concrete kernel alive for every
    // subsequent recompute()/recomputeFrom() call (ADR-M3-003, mirrors
    // ADR-010's externally-owned IRecomputable lifetime pattern).
    // PartDocument never constructs a kernel itself.
    void setGeometryKernel(IGeometryKernel* kernel) noexcept { kernel_ = kernel; }
    IGeometryKernel* geometryKernel() const noexcept { return kernel_; }

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

    // Shared box-feature registration/wiring logic for addBoxFeature and
    // restoreBoxFeature (single registration path, spec 13).
    void wireBoxFeature(BoxFeature& feature, ObjectId widthParameterId,
                       ObjectId heightParameterId, ObjectId depthParameterId,
                       ObjectId materialId);

    // Shared Pad registration/wiring for addPadFeature and restorePadFeature
    // (single registration path, spec 13).
    void wirePadFeature(PadFeature& feature, ObjectId sketchId, ObjectId lengthParameterId,
                       ObjectId materialId);

    // Detaches the MassPropertiesNode from whatever solid feature and material
    // it currently reads, then attaches it to this one. Shared by the Box and
    // Pad wiring paths so the graph can never accumulate stale prerequisites
    // from an earlier source.
    void rewireMassPropertiesSource(ObjectId solidFeatureId, ObjectId materialId);

    // Clears massProperties_.valid when the mass-properties node did not
    // succeed in the pass that produced `report`, so retained numbers can
    // never read as current. See the definition for why this must live at
    // document level rather than inside the node.
    void refreshMassPropertiesCurrency(const DocumentRecomputeReport& report) noexcept;

    // Demotes any Feature whose cached state() claims Valid while the graph
    // (the source of truth) disagrees. See the definition.
    void syncFeatureStatesFromGraph() noexcept;

    // Mutable lookup, private so every edit goes through editSketch().
    Sketch* findSketchForEdit(ObjectId id) noexcept;

    ParameterManager parameters_;
    std::vector<std::unique_ptr<Body>> bodies_;
    std::vector<std::unique_ptr<ReferenceFrame>> frames_;
    std::vector<std::unique_ptr<Connector>> connectors_;
    std::vector<std::unique_ptr<Sketch>> sketches_;
    std::shared_ptr<Material> material_;
    MassProperties massProperties_;
    ObjectRegistry registry_;
    DependencyGraph graph_;
    MassPropertiesNode massPropertiesNode_;
    IGeometryKernel* kernel_ = nullptr;
    DocumentRecomputeEngine engine_{*this};
};

} // namespace paramcad
