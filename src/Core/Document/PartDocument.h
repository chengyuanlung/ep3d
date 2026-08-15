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
class ISketchSolver;
class PadFeature;
class PocketFeature;
class RevolveFeature;
class FilletFeature;
class ChamferFeature;

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
    // Sets the expression (ParameterState -> Dirty) AND marks the graph node
    // dirty, mirroring setParameterValue exactly. Added in M8 round 3 when
    // Parameter's mutators went private (R1R3-M2): expression edits previously
    // had no facade path at all -- callers reached through parameters().items()
    // and bypassed dirty propagation.
    bool setParameterExpression(ObjectId id, std::string expression);
    // BREAKING vs M1: const-only. Use addParameter/setParameterValue/
    // removeObject for mutation (single registration path). Parameter's own
    // mutators are private since M8 round 3, so the Parameter* this hands out
    // can no longer edit past the facade.
    const ParameterManager& parameters() const noexcept { return parameters_; }

    // --- Bodies (registered; NO graph node in M2 -- nothing recomputes them)
    Body& addBody(std::string name);
    // Restore path (deserialization): adds a body that keeps its persisted id.
    Body& restoreBody(ObjectId id, std::string name);
    const std::vector<std::unique_ptr<Body>>& bodies() const noexcept { return bodies_; }

    ReferenceFrame& addFrame(std::string name, ObjectId parentFrameId = kInvalidObjectId);
    // KNOWN OPEN DOOR (M8 round 3, R1R3-M2, recorded not fixed): constness
    // stops at the unique_ptr, so this leaks mutable ReferenceFrame* with a
    // public setLocalTransform. Inert today -- no derived state reads frames,
    // so nothing can go stale -- and closed the sketches()/bodies()/Parameter
    // way the day a consumer appears. Connectors are the same shape.
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

    // Replacing the document's Material unhooks the OUTGOING one from the
    // registry and the graph first. Without that, the old Material is destroyed
    // (its shared_ptr use_count is 1) while the registry still resolves its id
    // to the freed address -- and MassPropertiesNode::resolveMaterial then
    // reads density out of freed memory. In Release that returned the STALE
    // value and the document reported a plausible but wrong mass as CURRENT,
    // with RecomputeStatus::Success and no diagnostic anywhere.
    void detachCurrentMaterial() noexcept;

    Material& restoreMaterial(ObjectId id, std::string name, double densityKgPerM3,
                              double elasticModulusPa, double poissonRatio,
                              double yieldStrengthPa, ContactProperties contact);
    // Sets density (no validation here -- MassPropertiesNode::recompute
    // validates finite/non-negative, ADR-M3-005 density policy) AND marks the
    // graph node dirty, mirroring setParameterValue exactly. False if no
    // material is assigned.
    bool setMaterialDensity(double densityKgPerM3);
    // const Material*, NOT the shared_ptr (M8 round 3, R1R3-M2): constness
    // stopped at the shared_ptr, and setDensity through a const document
    // left mass reading the old density as current. Ownership never leaves
    // the document; readers get a pointer they cannot mutate through.
    const Material* material() const noexcept { return material_.get(); }

    // NOTE: the non-const overload lets any caller overwrite derived state,
    // which sits awkwardly with "mutation goes through the facade" below.
    // MassPropertiesNode is its only legitimate writer. Left public
    // deliberately: every read through a non-const PartDocument selects this
    // overload too, so restricting it churns ~20 unrelated call sites for no
    // behavioural gain. Candidate M4 cleanup alongside the ADR-M3-004
    // Feature/IRecomputable collapse.
    // The auto-created mass-properties node's id. It is a document object like
    // any other -- registered, graph-scheduled, removable -- so callers that
    // need to name it should not have to guess.
    ObjectId massPropertiesNodeId() const noexcept;

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


    // --- Sketch (M4, ADR-M4-001/002/005; M5) -------------------------------
    // Creates a Sketch, registers it, and adds a graph node.
    //
    // In M4 a Sketch was a DIRTY SOURCE like Parameter and Material, because it
    // had no derived state of its own. Since M5 it does -- solved geometry is
    // derived from its constraints and their bound Parameters -- so it is a
    // RECOMPUTABLE node instead. A constraint-free sketch behaves exactly as it
    // did in M4: its recompute succeeds without touching geometry.
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

    // --- Sketch constraints (M5) -------------------------------------------
    // THE path for adding a constraint. Adds it to the sketch AND wires the
    // graph edge from any Parameter the constraint binds, so a dimension edit
    // propagates through the existing M2 machinery rather than through anything
    // new. Adding a constraint straight to the Sketch would leave the graph
    // unaware that the sketch now depends on that Parameter -- the same hazard
    // editSketch exists to remove.
    //
    // Returns kInvalidSketchConstraintId if the sketch id is unknown or the
    // sketch rejects the constraint; in that case no edge is wired and nothing
    // is dirtied.
    SketchConstraintId addSketchConstraint(ObjectId sketchId, SketchConstraintData data);

    // Removes a constraint, dropping the Parameter edge only if no remaining
    // constraint on that sketch still binds the same Parameter.
    bool removeSketchConstraint(ObjectId sketchId, SketchConstraintId constraintId);

    // Removes a sketch entity, cascading to every constraint referencing it
    // (ADR-M5-009) and dropping any Parameter edge those constraints were the
    // last to hold. False if the sketch or the entity is unknown.
    //
    // Sketch::removeEntity does the cascade but cannot touch the graph, so this
    // is the path callers should use -- the Parameter would otherwise keep
    // dirtying a sketch that no longer reads it.
    bool removeSketchEntity(ObjectId sketchId, SketchEntityId entityId);

    // Constraints anywhere in this document that bind `parameterId`. Empty if
    // none do, which is exactly the condition under which the Parameter can be
    // deleted (see removeObject).
    std::vector<SketchConstraintId> constraintsBindingParameter(ObjectId parameterId) const;

    // Features that read this sketch. Empty is exactly the condition under
    // which the sketch can be deleted (see removeObject).
    std::vector<ObjectId> featuresReferencingSketch(ObjectId sketchId) const;

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

    // --- Placeholder feature (facade since M8 round 2) ----------------------
    // Placeholders keep their original semantics -- no registry entry, no
    // graph node, inert (ADR-009 D4) -- but arrive through the facade like
    // every other feature, because Body::addFeature going public was the
    // bypass R2R2-M1 drove a rogue consumer through. The restore path is
    // guarded like its six siblings (duplicate-id refusal, round 3): a
    // caller inventory in a comment enforces nothing, so the guard does.
    class PlaceholderFeature& addPlaceholderFeature(Body& body, std::string name,
                                                    std::string typeName);
    class PlaceholderFeature& restorePlaceholderFeature(Body& body, ObjectId id,
                                                        std::string name, ComputeState state,
                                                        std::string typeName);

    // --- Pocket feature (M8, ADR-M8-001) -----------------------------------
    // The first CONSUMING feature: removes material from `baseFeatureId`'s
    // result. Wires the chain edge base -> pocket alongside the Sketch and
    // Depth edges, and re-points the document's MassPropertiesNode at the
    // pocket -- the chain TAIL is what downstream physics reads (ADR-M8-003).
    PocketFeature& addPocketFeature(Body& body, std::string name, ObjectId baseFeatureId,
                                    ObjectId sketchId, ObjectId depthParameterId);
    PocketFeature& restorePocketFeature(Body& body, ObjectId id, std::string name,
                                        ComputeState state, ObjectId baseFeatureId,
                                        ObjectId sketchId, ObjectId depthParameterId,
                                        ObjectId materialId);

    // --- Revolve feature (M8.2, ADR-M8-005) --------------------------------
    // Base-capable like Pad: revolves `sketchId`'s profile about the sketch's
    // own line `axisEntityId` by the Radian Parameter `angleParameterId`.
    RevolveFeature& addRevolveFeature(Body& body, std::string name, ObjectId sketchId,
                                      SketchEntityId axisEntityId, ObjectId angleParameterId);
    RevolveFeature& restoreRevolveFeature(Body& body, ObjectId id, std::string name,
                                          ComputeState state, ObjectId sketchId,
                                          SketchEntityId axisEntityId,
                                          ObjectId angleParameterId, ObjectId materialId);

    // --- Fillet / Chamfer (M8.3, ADR-M8-006) -------------------------------
    // Edge-dressing consumers: every edge of `baseFeatureId`'s result, by one
    // length Parameter. Chain semantics identical to Pocket's.
    FilletFeature& addFilletFeature(Body& body, std::string name, ObjectId baseFeatureId,
                                    ObjectId radiusParameterId);
    FilletFeature& restoreFilletFeature(Body& body, ObjectId id, std::string name,
                                        ComputeState state, ObjectId baseFeatureId,
                                        ObjectId radiusParameterId, ObjectId materialId);
    ChamferFeature& addChamferFeature(Body& body, std::string name, ObjectId baseFeatureId,
                                      ObjectId distanceParameterId);
    ChamferFeature& restoreChamferFeature(Body& body, ObjectId id, std::string name,
                                          ComputeState state, ObjectId baseFeatureId,
                                          ObjectId distanceParameterId, ObjectId materialId);

    // Non-owning; the caller keeps the concrete kernel alive for every
    // subsequent recompute()/recomputeFrom() call (ADR-M3-003, mirrors
    // ADR-010's externally-owned IRecomputable lifetime pattern).
    // PartDocument never constructs a kernel itself.
    // Both setters DIRTY the nodes that depend on the backend they replace.
    //
    // Without that, correcting the input never cleared the failure: a sketch
    // that recomputed with no solver is left Failed, and DependencyGraph skips
    // any node that is not Dirty, so injecting the real solver afterwards left
    // the sketch -- and every feature downstream of it -- permanently blocked.
    // ADR-M5-004 promises that "correcting the input and recomputing" recovers;
    // for this input it did not, and "load a file, then inject the backend" is
    // an ordinary ordering because loadPartDocument returns a document with
    // neither backend set.
    void setGeometryKernel(IGeometryKernel* kernel) noexcept;
    IGeometryKernel* geometryKernel() const noexcept { return kernel_; }

    // Same non-owning contract as the kernel, for the same reason (ADR-M3-003
    // extended to M5): the caller owns the concrete solver and keeps it alive
    // for every subsequent recompute. PartDocument never constructs one, so
    // Core keeps no dependency on Eigen (ADR-M5-003).
    void setSketchSolver(ISketchSolver* solver) noexcept;
    ISketchSolver* sketchSolver() const noexcept { return sketchSolver_; }

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
    void wirePocketFeature(PocketFeature& feature, ObjectId baseFeatureId, ObjectId sketchId,
                           ObjectId depthParameterId, ObjectId materialId);
    void wireRevolveFeature(RevolveFeature& feature, ObjectId sketchId,
                            ObjectId angleParameterId, ObjectId materialId);
    void wireEdgeDressFeature(class EdgeDressFeature& feature, ObjectId baseFeatureId,
                              ObjectId sizeParameterId, ObjectId materialId);
    void wirePadFeature(PadFeature& feature, ObjectId sketchId, ObjectId lengthParameterId,
                       ObjectId materialId);

    // The chain rule at the door (ADR-M8-001, amended by review round 1's
    // R1-C1/R1-M2): a consumer's base must be a SOLID feature of the SAME
    // body, and a solid may be consumed ONCE. Without this, two pockets on
    // one pad were silently accepted -- both cut the original pad, the viewer
    // drew two overlapping solids, mass followed whichever consumer wired
    // last, and the file round-tripped. Throws std::runtime_error, exactly as
    // the restore paths' duplicate-id guards do; the loader's chain walk
    // refuses the same shapes before any restore call, so via loadPartDocument
    // this surfaces as a LoadResult failure, never an exception.
    void requireConsumableBase(const Body& body, ObjectId baseFeatureId,
                               const char* consumerNoun) const;

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

    // Makes the graph's Parameter -> Sketch edges match the sketch's CURRENT
    // constraint set exactly: adds what is bound, removes what is not.
    //
    // It OWNS every Parameter -> Sketch edge. One added by hand through
    // addDependency is therefore revoked by the next recompute pass -- correct,
    // because such an edge has no constraint behind it and the loader would not
    // re-derive it, so keeping it would make the document behave differently
    // before and after a save/load. Stated here because it is surprising: the
    // call succeeds and the edge later disappears.
    //
    // Every mutation path calls this rather than each adjusting edges itself.
    // The per-path version left two holes that independent review found: a
    // constraint added through editSketch (which hands out a mutable Sketch&,
    // and which the shipped viewer uses) wired no edge at all, so the document
    // behaved differently before and after a save/load -- the loader re-derives
    // edges from the constraints, so one appeared out of nowhere. And a
    // cascaded entity removal through the same path left a PHANTOM edge, so a
    // Parameter kept re-solving a sketch that no longer read it.
    void reconcileSketchParameterEdges(ObjectId sketchId);

    // Reconciles every sketch. Called at the start of each recompute pass as a
    // NET, not as the primary mechanism: `addSketch` hands back a mutable
    // `Sketch&`, so `Sketch::addConstraint` is reachable without going through
    // any facade at all -- a fifth mutation path ADR-M5-013 did not list and
    // the header two screens up claims does not exist. Through it a dimension
    // edit silently did nothing, and the document behaved differently before
    // and after a save/load, because the loader re-derives edges and the live
    // document had none.
    //
    // Reconciling per pass makes the graph agree with the constraint set
    // whatever route a caller took, at the cost of one walk over the sketches.
    void reconcileAllSketchParameterEdges();

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
    ISketchSolver* sketchSolver_ = nullptr;
    DocumentRecomputeEngine engine_{*this};
};

} // namespace paramcad
