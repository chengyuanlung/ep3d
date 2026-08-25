#pragma once

#include "Core/Body/Body.h"
#include "Core/Connector/Connector.h"
#include "Core/Dependency/DependencyGraph.h"
#include "Core/Document/DocumentBase.h"
#include "Core/Document/ObjectRegistry.h"
#include "Core/Expression/ExpressionTypes.h"
#include "Core/Feature/TransformFeatures.h"
#include "Core/Undo/UndoRecord.h"
#include "Core/Material/Material.h"
#include "Core/Parameter/ParameterManager.h"
#include "Core/Feature/BooleanFeature.h"
#include "Core/Kernel/FaceQuery.h"
#include "Core/Physics/MassProperties.h"
#include "Core/Physics/MassPropertiesNode.h"
#include "Core/Recompute/DocumentRecomputeEngine.h"
#include "Core/Recompute/RecomputeTypes.h"
#include "Core/Geometry/MathTypes.h"
#include "Core/Reference/ReferenceFrame.h"
#include "Core/Sketch/Sketch.h"
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <string>
#include <vector>

namespace paramcad {

class IRecomputable;
class IGeometryKernel;
class BoxFeature;
class ISketchSolver;
class PadFeature;
class PocketFeature;
class RevolveFeature;
class SweepFeature;
class LoftFeature;
class ShellFeature;
class DraftFeature;
class HoleFeature;
// M39: a hole names a screw, and the names live in one header.
class BooleanFeature;
class ImportFeature;
class CircularPatternFeature;
class CurvePatternFeature;
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
class PartDocument final : public DocumentBase {
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
    //
    // M11.2 -- this is now a VALIDATING facade, not a setter. An expression is
    // parsed, evaluated once against the document's current parameter values,
    // and its `#name` references become dependency-graph edges. Any failure
    // leaves the document byte-for-byte unchanged and returns false, filling
    // `error` (which carries a character POSITION, roadmap section 42.3.3) when
    // one is supplied. Refused cases:
    //
    //   * the text does not parse;
    //   * it references a name no parameter has, or an AMBIGUOUS name two
    //     parameters share;
    //   * it produces the wrong dimension for this parameter's unit, or the
    //     unit has no expression dimension at all (ExpressionDimensionOf);
    //   * the resulting edges would close a CYCLE -- the message names the
    //     path.
    //
    // An empty (or all-whitespace) expression CLEARS: edges are removed and the
    // parameter goes back to holding a literal value. That is the documented
    // way to undo `#width / 2` without deleting the parameter.
    bool setParameterExpression(ObjectId id, std::string expression,
                                ExpressionError* error = nullptr);

    // Parameters whose expression reads `parameterId`. Empty is exactly the
    // condition under which the Parameter can be deleted, mirroring
    // constraintsBindingParameter and ADR-M5-009: a named, shared,
    // document-level object is REFUSED rather than cascaded, because silently
    // breaking someone's formulas destroys more than was asked for.
    std::vector<ObjectId> parametersReferencingParameter(ObjectId parameterId) const;

    // Rebuild every expression's dependency edges from the stored text.
    //
    // The LOADER's counterpart to setParameterExpression: edges are not
    // persisted (the graph is rebuilt on load), so a document whose parameters
    // reference each other arrives with the text and none of the wiring, and a
    // recompute would then evaluate them in the wrong order. Called ONCE, after
    // every parameter is restored, so forward references resolve.
    //
    // Records NO undo step (ADR-M9-001: a loaded document starts with an empty
    // history). `ok == false` means the file is not loadable as written; the
    // loader refuses rather than opening a document that cannot be recomputed.
    struct ExpressionWiringResult {
        bool ok{true};
        ObjectId parameterId{kInvalidObjectId};
        std::string message;
    };
    ExpressionWiringResult rewireParameterExpressions();

    // The same checks as rewireParameterExpressions, minus the wiring, minus
    // the cycle (which only wiring can reveal). This is the SAVE-side net: a
    // file whose stored expression cannot be resolved is one this loader would
    // refuse, and a save that emits an unopenable document is worse than a save
    // that fails (the precedent is validateSaveable's id-cap check).
    ExpressionWiringResult validateParameterExpressions() const;

    // Resolve `name` for expression evaluation. nullopt when no parameter has
    // that name, when TWO do, or when the parameter's unit has no expression
    // dimension -- three different reasons the caller cannot act on
    // differently at this point, and which setParameterExpression distinguishes
    // before an expression is ever stored.
    std::optional<Quantity> resolveExpressionVariable(std::string_view name) const override;
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
    // The body called `name`, or nullptr (M20).
    //
    // Bodies are named by the user, so this is how a command that dresses "the
    // part" finds the one an earlier command built. It hands back a mutable
    // Body because every addXFeature takes one -- a named door rather than
    // reaching through the const-stops-at-the-pointer gap `bodies()` leaves,
    // which the note below calls an open door and means to keep shut.
    Body* findBodyNamed(const std::string& name) noexcept;

    // KNOWN OPEN DOOR (M8 round 3, R1R3-M2, recorded not fixed): constness
    // stops at the unique_ptr, so this leaks mutable ReferenceFrame* with a
    // public setLocalTransform. Inert today -- no derived state reads frames,
    // so nothing can go stale -- and closed the sketches()/bodies()/Parameter
    // way the day a consumer appears. Connectors are the same shape.
    // Const-correct since M10: the mutable-pointee door round 3 recorded as
    // "inert -- and closed the day a consumer appears" is closed, because a
    // sketch now reads its support frame and a transform changed behind the
    // facade would leave the graph undirtied.




    // --- Sketch on frame (M10.2, ADR-M10-003) -------------------------------
    //
    // Puts `sketchId` on `frameId`, wiring the frame -> sketch graph edge so a
    // frame move dirties the sketch and everything built on it. Pass
    // kInvalidObjectId to take the sketch off its frame and back onto its own
    // embedded plane. False if either id is wrong.
    bool setSketchSupportFrame(ObjectId sketchId, ObjectId frameId);
    // The restore-path twin: records no undo step (ADR-M9-001).
    bool restoreSketchSupportFrame(ObjectId sketchId, ObjectId frameId);


    // The plane a sketch's (u,v) actually lives on: its support frame's WORLD
    // transform when it has one, otherwise its own embedded SketchFrame.
    //
    // The one resolution site. A feature asks this rather than reading
    // `sketch.frame()`, because reading the embedded frame directly is exactly
    // how a sketch would silently stay at the origin after its frame moved.
    SketchFrame effectiveSketchFrame(ObjectId sketchId) const noexcept;

    // True when the sketch NAMES a support frame that is not in this document.
    //
    // Separate from `effectiveSketchFrame` because that function returns a
    // value and a value cannot say "there is no answer". Without this, deleting
    // a support frame made `worldTransform` return identity for the dead id and
    // the sketch silently relocated to world XY -- geometry moving on its own,
    // which is the geometric twin of the stale-result defect this project has
    // fixed three times. Callers fail loudly on true (M10 gate I).
    bool sketchSupportFrameIsMissing(ObjectId sketchId) const noexcept;

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

    // --- Sketch geometry through the facade (M12) --------------------------
    //
    // THE path for adding an entity once a USER can add one. `addSketch` hands
    // back a mutable `Sketch&` and `Sketch::addLine` still works, but that path
    // records no undo delta -- which was invisible while sketches were built in
    // code and became the first defect a mouse-driven tool hits.
    //
    // Returns kInvalidSketchEntityId if the sketch id is unknown or the
    // geometry is degenerate; in that case nothing is recorded and nothing is
    // dirtied.
    // `construction` creates it as construction geometry in ONE step, which is
    // what a polygon's circumscribed circle needs (M17.17). Doing it by calling
    // setSketchEntitiesConstruction afterwards is a trap: that facade opens a
    // ScopedTransaction, and from inside a caller's open transaction its
    // destructor commits the CALLER's.
    SketchEntityId addSketchEntity(ObjectId sketchId, SketchGeometry geometry,
                                   bool construction = false);

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

    // --- Dimension placement (M16) -----------------------------------------
    //
    // THE recording path for where a dimension's value sits. Passing no point
    // puts it back on automatic placement.
    //
    // A drag must NOT call this per mouse move: it would push one delta per
    // pixel into the open transaction. The canvas previews through
    // `editSketch` and calls this ONCE on release, so the whole drag is one
    // undo step (roadmap section 15: one user action = one undo).
    // --- Dragging geometry under its constraints (M17) ----------------------
    //
    // A drag is one question asked repeatedly: "hold THIS point at the cursor
    // and solve everything else". The pin is a pair of residuals appended to
    // the problem, NOT a constraint added to the document -- a real Fix would
    // have to be created and destroyed on every mouse move, would land on the
    // undo stack, and would be visible in the constraint list while the mouse
    // was down.
    //
    // The constraints decide what happens. A point on a line slides along it, a
    // fully constrained sketch does not move, and a drag that would break the
    // model simply does not converge -- all of that is the solver's answer, not
    // a rule written here. What this must never do is move the geometry when
    // the solve failed: `false` means nothing was touched.
    //
    // Records NOTHING and dirties NOTHING. A drag across the canvas is one
    // action, and a thousand undo steps for it would be a history nobody can
    // get back through -- the same reason dimension dragging previews
    // (ADR-M16). `commitSketchDrag` is what makes it permanent.
    // Returns the SOLVER'S verdict, not a bool.
    //
    // "It did not move" is not an answer a user can act on (roadmap 8). A
    // conflicting drag, an under-constrained one and a reference the solver has
    // no variable for are three different situations, and the status is what
    // lets the canvas say which. Geometry is written only for a converged
    // result; InvalidInput means the reference itself was undraggable.
    SketchSolveStatus previewSketchDrag(ObjectId sketchId, const SketchElementRef& point,
                                        Vec2 targetMm);

    // Everything in the sketch, as it stands. Taken when a drag starts, so the
    // drag can be committed as a delta or rolled back wholesale.
    std::vector<std::pair<SketchEntityId, SketchGeometry>> sketchGeometrySnapshot(
        ObjectId sketchId) const;

    // Makes a drag permanent: one undo step carrying every entity that MOVED.
    //
    // Every entity, not just the one under the cursor -- a drag under
    // constraints moves the neighbours too, and an undo that put back only the
    // grabbed point would leave the sketch in a state the user never saw.
    // Returns how many entities changed; 0 means nothing moved and nothing is
    // recorded.
    std::size_t commitSketchDrag(
        ObjectId sketchId, const std::vector<std::pair<SketchEntityId, SketchGeometry>>& before,
        const std::string& label);

    // Puts a snapshot back without recording anything. Esc during a drag.
    bool restoreSketchGeometry(
        ObjectId sketchId, const std::vector<std::pair<SketchEntityId, SketchGeometry>>& before);

    // Reshapes an entity in place, keeping its id and every constraint on it.
    //
    // This is what a trim, an extend or a chamfer setback is made of. The
    // alternative -- delete and recreate -- issues a new id and cascades away
    // every constraint the old one carried (ADR-M5-009), so a trimmed line
    // would come back joined to nothing.
    //
    // Dirties the sketch: the shape feeds the profile and every feature
    // downstream of it.
    bool setSketchEntityGeometry(ObjectId sketchId, SketchEntityId entityId,
                                 SketchGeometry geometry);

    // Switches entities to or from construction geometry, as ONE undo step
    // (roadmap 4.1.1).
    //
    // Takes a LIST because the command acts on a selection and a user who
    // switched five lines expects one Ctrl+Z to bring all five back. Dirties
    // the sketch: the flag changes what BuildProfile sees, so every feature
    // downstream of this sketch has to be recomputed even though no coordinate
    // moved. Returns how many actually changed.
    std::size_t setSketchEntitiesConstruction(ObjectId sketchId,
                                              const std::vector<SketchEntityId>& entityIds,
                                              bool construction);

    // Gives a sketch its projected reference underlay (M17.6, ADR-M17-029).
    // Returns how many were accepted; degenerate geometry is refused entry.
    //
    // Through the FACADE like every other sketch mutation (UI spec 20), even
    // though a reference has no graph consequence: the shell writing straight
    // into a Sketch is the bypass this project has already had to close once,
    // and "this particular write is harmless" is how the next one gets written.
    //
    // NOT an undo step, and consistent with the sketch itself -- creating a
    // sketch is not undoable either, and an underlay that could be undone out
    // from under its sketch would leave a face sketch that no longer knows
    // what it was made on.
    // Replaces WHICH edges a Fillet or Chamfer dresses, and DIRTIES it
    // (M17.13, ADR-M17-035). False for an id that is not an edge-dressing
    // feature.
    //
    // The dirtying is the whole reason this exists: the selection decides what
    // the feature produces, so a change to it that left the graph clean would
    // be skipped by the next recompute and the old shape would be handed back
    // as current.
    // Renames anything the tree shows a name for -- a sketch, a feature, a
    // parameter, a body, the material (M17.16, ADR-M17-039). ONE undo step.
    //
    // REFUSES a duplicate, because names are how a user picks what to delete
    // or edit -- and for a parameter a duplicate is a correctness problem, not
    // a cosmetic one: expressions resolve by name and findByName answers with
    // the first match (ADR-M17-038).
    //
    // Does NOT dirty anything. A name has no geometric consequence, and
    // marking the object dirty would rebuild the whole chain below it to
    // produce identical shapes.


    // Publishes what a DRIVEN dimension measured (M17.19, ADR-M17-042).
    //
    // NOT an undo step, unlike setParameterValue: nobody typed this. It is a
    // derived value, republished on every recompute, and recording it would
    // fill the history with steps a user cannot recognise and undoing one
    // would only be overwritten by the next solve.
    //
    // It also does NOT clear an expression, for the same reason -- but a
    // driven dimension should not have one, and the panel refuses to give it
    // one.
    // Switches a dimension between DRIVING and MEASURING (M17.19,
    // ADR-M17-042). One undo step, and it dirties the sketch: the problem the
    // solver is given is a different problem afterwards.
    bool setSketchConstraintDriven(ObjectId sketchId, SketchConstraintId constraintId,
                                   bool driven);

    bool setDrivenParameterValue(ObjectId parameterId, double value);

    bool setFeatureEdgeSelection(ObjectId featureId, EdgeSelection selection);

    // WHAT SCREW A HOLE IS FOR, AND WHAT SHAPE ITS MOUTH IS (M39).
    //
    // Both mark the feature dirty for the same reason setFeatureEdgeSelection
    // does: the shape this feature produces has changed even though no
    // parameter did. Saying "M8, tapped" over a hole that was 12 mm across
    // redrills it to 6.8.
    //
    // REFUSED HERE if the designation is one this build has no numbers for, so
    // the document never holds a hole that cannot recompute. The alternative
    // is a hole that goes red the next time anything is edited, pointing at a
    // thread the user typed some while ago.
    bool setHoleKind(ObjectId featureId, HoleKind kind);
    bool setHoleScrew(ObjectId featureId, HoleScrew screw);

    // Makes a sketch TRACK a face instead of sitting on a frozen plane
    // (M17.14, ADR-M17-036), and adds the graph edge that makes it true.
    //
    // The edge is the point. A tracked sketch depends on the feature whose
    // face it is on: move the pad and the sketch must be re-resolved BEFORE
    // anything downstream reads its plane. Setting the query without the edge
    // would leave the sketch clean when the pad moved, so it would report the
    // plane from before the move and every feature built on it would be built
    // in the wrong place -- while every state in the model said it was fine.
    //
    // Refuses a query naming a feature that is not a solid, or one that would
    // close a cycle (a sketch tracking a face of something built from itself).
    bool setSketchTrackedFace(ObjectId sketchId, FaceQuery query);

    std::size_t addSketchReferences(ObjectId sketchId,
                                    const std::vector<SketchGeometry>& geometry);

    bool setSketchDimensionPlacement(ObjectId sketchId, SketchConstraintId constraintId,
                                     Vec2 labelMm);
    bool clearSketchDimensionPlacement(ObjectId sketchId, SketchConstraintId constraintId);

    // The LIVE half of a drag: moves a dimension's value without recording an
    // undo step and WITHOUT dirtying the sketch.
    //
    // `editSketch` cannot serve here, and that difference is the whole reason
    // this exists: editSketch marks the sketch dirty, which is right for
    // geometry and wrong for a label. Using it for the drag preview made every
    // mouse move stale the sketch and everything downstream of it -- the model
    // tree read "Needs recompute" because a number had been slid four
    // millimetres. Where a value SITS is not an input to anything.
    //
    // The recording, undoable move is `setSketchDimensionPlacement`, which the
    // canvas calls once on release.
    bool previewSketchDimensionPlacement(ObjectId sketchId, SketchConstraintId constraintId,
                                         Vec2 labelMm);
    bool previewClearSketchDimensionPlacement(ObjectId sketchId,
                                              SketchConstraintId constraintId);

    // THE recording path for how a dimension's value reads. A default-valued
    // format clears any override.
    bool setSketchDimensionFormat(ObjectId sketchId, SketchConstraintId constraintId,
                                  const Sketch::DimensionFormat& format);

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
    // AN IMPORT reads a solid from a STEP file. A chain BASE, like a Box.
    ImportFeature& addImportFeature(Body& body, std::string name, std::string path);
    ImportFeature& restoreImportFeature(Body& body, ObjectId id, std::string name,
                                        ComputeState state, std::string path,
                                        ObjectId materialId);

    // A BOOLEAN combines TWO solids -- the first feature here to consume two.
    BooleanFeature& addBooleanFeature(Body& body, std::string name, BooleanOperation operation,
                                      ObjectId targetFeatureId, ObjectId toolFeatureId);
    BooleanFeature& restoreBooleanFeature(Body& body, ObjectId id, std::string name,
                                          ComputeState state, BooleanOperation operation,
                                          ObjectId targetFeatureId, ObjectId toolFeatureId,
                                          ObjectId materialId);

    // A CIRCULAR pattern turns copies about a frame's local +Z.
    CircularPatternFeature& addCircularPatternFeature(Body& body, std::string name,
                                                      ObjectId baseFeatureId, ObjectId frameId,
                                                      ObjectId countParameterId,
                                                      ObjectId stepParameterId);
    CircularPatternFeature& restoreCircularPatternFeature(
        Body& body, ObjectId id, std::string name, ComputeState state, ObjectId baseFeatureId,
        ObjectId frameId, ObjectId countParameterId, ObjectId stepParameterId,
        ObjectId materialId);

    // A CURVE pattern spaces copies along a sketch's path.
    CurvePatternFeature& addCurvePatternFeature(Body& body, std::string name,
                                                ObjectId baseFeatureId, ObjectId pathSketchId,
                                                ObjectId countParameterId);
    CurvePatternFeature& restoreCurvePatternFeature(Body& body, ObjectId id, std::string name,
                                                    ComputeState state, ObjectId baseFeatureId,
                                                    ObjectId pathSketchId,
                                                    ObjectId countParameterId,
                                                    ObjectId materialId);

    // A SHELL hollows a base and opens the faces its selection names.
    ShellFeature& addShellFeature(Body& body, std::string name, ObjectId baseFeatureId,
                                  FaceSelection openFaces, ObjectId thicknessParameterId);
    ShellFeature& restoreShellFeature(Body& body, ObjectId id, std::string name,
                                      ComputeState state, ObjectId baseFeatureId,
                                      FaceSelection openFaces, ObjectId thicknessParameterId,
                                      ObjectId materialId);

    // A DRAFT tapers faces about a neutral one, which also gives the pull.
    DraftFeature& addDraftFeature(Body& body, std::string name, ObjectId baseFeatureId,
                                  FaceSelection faces, FaceQuery neutral,
                                  ObjectId angleParameterId);
    DraftFeature& restoreDraftFeature(Body& body, ObjectId id, std::string name,
                                      ComputeState state, ObjectId baseFeatureId,
                                      FaceSelection faces, FaceQuery neutral,
                                      ObjectId angleParameterId, ObjectId materialId);

    // A HOLE drills at a sketch's points, through a base.
    HoleFeature& addHoleFeature(Body& body, std::string name, ObjectId baseFeatureId,
                                ObjectId sketchId, ObjectId diameterParameterId,
                                ObjectId depthParameterId);
    HoleFeature& restoreHoleFeature(Body& body, ObjectId id, std::string name, ComputeState state,
                                    ObjectId baseFeatureId, ObjectId sketchId,
                                    ObjectId diameterParameterId, ObjectId depthParameterId,
                                    ObjectId materialId);

    // A SWEEP takes two sketches -- a section and a spine -- and depends on
    // both. See SweepFeature.h for why they cannot be one.
    SweepFeature& addSweepFeature(Body& body, std::string name, ObjectId profileSketchId,
                                  ObjectId pathSketchId);
    SweepFeature& restoreSweepFeature(Body& body, ObjectId id, std::string name,
                                      ComputeState state, ObjectId profileSketchId,
                                      ObjectId pathSketchId, ObjectId materialId);

    // A LOFT takes two or more sections IN ORDER, and depends on every one.
    LoftFeature& addLoftFeature(Body& body, std::string name,
                                std::vector<ObjectId> sectionSketchIds);
    LoftFeature& restoreLoftFeature(Body& body, ObjectId id, std::string name, ComputeState state,
                                    std::vector<ObjectId> sectionSketchIds, ObjectId materialId);

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

    // --- Mirror / Pattern (M10.6, ADR-M9-006's deferral closed) -------------
    // Consuming features like Pocket, so they inherit the whole chain: base by
    // ObjectId, consumed once, suppression semantics, tail display. The frame
    // supplies the mirror plane (its XY) and the pattern direction (its +X).
    MirrorFeature& addMirrorFeature(Body& body, std::string name, ObjectId baseFeatureId,
                                    ObjectId frameId);
    MirrorFeature& restoreMirrorFeature(Body& body, ObjectId id, std::string name,
                                        ComputeState state, ObjectId baseFeatureId,
                                        ObjectId frameId, ObjectId materialId);
    PatternFeature& addPatternFeature(Body& body, std::string name, ObjectId baseFeatureId,
                                      ObjectId frameId, ObjectId countParameterId,
                                      ObjectId spacingParameterId);
    PatternFeature& restorePatternFeature(Body& body, ObjectId id, std::string name,
                                          ComputeState state, ObjectId baseFeatureId,
                                          ObjectId frameId, ObjectId countParameterId,
                                          ObjectId spacingParameterId, ObjectId materialId);

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
    void setGeometryKernel(IGeometryKernel* kernel) noexcept override;

    // Same non-owning contract as the kernel, for the same reason (ADR-M3-003
    // extended to M5): the caller owns the concrete solver and keeps it alive
    // for every subsequent recompute. PartDocument never constructs one, so
    // Core keeps no dependency on Eigen (ADR-M5-003).
    void setSketchSolver(ISketchSolver* solver) noexcept override;

    // --- Feature activity: suppression and rollback (M9.3 / M9.4) -----------
    //
    // ONE concept for two features of the milestone, because the document
    // treats them identically: an INACTIVE feature is not evaluated, not
    // displayed, and not a candidate for the chain tail. It is inactive when it
    // is Suppressed (the user turned it off) or when it lies beyond its body's
    // rollback position (the user is looking at an earlier step).
    //
    // What inactive is NOT is *failed*. A failed feature's result is wrong or
    // missing and the model is broken; an inactive one is exactly what the user
    // asked for. M8's review round 1 found that treating a FAILED consumer as
    // "not consuming" let the viewer show a healthy-looking wrong solid, and
    // consumption became structural because of it. Suppression flips that back
    // deliberately and only for the deliberate case -- the difference is the
    // whole of ADR-M9-002.
    bool isFeatureActive(ObjectId featureId) const noexcept;

    // The nearest ACTIVE solid at or above `baseFeatureId` in the chain, or
    // kInvalidObjectId when the chain runs out.
    //
    // This is how suppression "closes the chain" without rewriting anything:
    // a consumer keeps its stored base reference for ever -- the reference is
    // what the model says -- and resolution walks past inactive links at
    // recompute time. Suppress the Pocket in Sketch -> Pad -> Pocket -> Fillet
    // and the Fillet dresses the PAD; unsuppress it and the Fillet dresses the
    // pocket again, with no edit to either feature (ADR-M9-002).
    ObjectId activeChainBase(ObjectId baseFeatureId) const noexcept;

    // --- Rollback position (M9.4, ADR-M9-004) -------------------------------
    //
    // `cut` is the number of features evaluated: 0 hides all of them,
    // features().size() (or Body::kNoRollback) evaluates everything. Dirties
    // what changed and re-points the mass source at the new tail. False if the
    // id is not a Body in this document.
    bool setRollbackPosition(ObjectId bodyId, std::size_t cut);
    // The restore-path twin: same effect, records NO undo step. Deserialization
    // is not a user edit, and a loaded document must start with an empty
    // history rather than with the history of its own construction
    // (ADR-M9-001, pinned by M9_UNDO_402).
    bool restoreRollbackPosition(ObjectId bodyId, std::size_t cut);
    std::size_t rollbackPosition(ObjectId bodyId) const noexcept;

    // --- Undo / redo (M9.1, ADR-M9-001) -------------------------------------
    //
    // Every edit through this facade that CAN be replayed semantically becomes
    // an undo record: parameter value and expression edits, and the addition or
    // removal of a feature. Undo re-executes the inverse through this same
    // facade, so the registry, the graph and the dirty set end up exactly where
    // an equivalent hand edit would have left them -- and the geometry is
    // whatever the next recompute derives, never a shape held in the history.
    //
    // Undo does NOT recompute, deliberately, exactly as `setParameterValue`
    // does not. The caller decides when to rebuild, which is what lets a test
    // (and the shell) prove selectivity by counters.
    //
    // NOT RECORDED, and why each is honest rather than lazy:
    //   * `restore*` paths -- deserialization is not a user edit, and a loaded
    //     document starts with an empty history rather than with someone
    //     else's session's.
    //   * removing a Body, Parameter, Sketch or Material, and removing a
    //     feature that another feature CONSUMES. None of these can be replayed
    //     faithfully yet, and a history that silently does the wrong thing is
    //     worse than no history: these operations CLEAR both stacks. The
    //     clearing is observable (`undoDepth()` drops to zero), so a UI can
    //     tell the user rather than offering an undo that would lie.


    // --- Recompute infrastructure facade -----------------------------------
    // Registers an externally owned recomputable (e.g. a test stub) and gives
    // it a graph node. The caller guarantees the object outlives its
    // registration (remove with removeObject).

    GraphResult setSuppressed(ObjectId id, bool suppressed);
    DocumentRecomputeReport recompute() override;
    // Graph markDirty plus the ADR-011 bridge: a Parameter node also gets
    // its evaluation state marked dirty.
    bool markDirty(ObjectId id) override;

    // markDirty(id) + full dirty-set recompute (see DocumentRecomputeEngine).
    DocumentRecomputeReport recomputeFrom(ObjectId id) override;

    // Removes a registered object everywhere, in this order: graph node
    // (edges cleaned, former dependents dirtied per ADR-007) -> registry ->
    // owning container (Parameter/Body; externally owned IRecomputables have
    // no owner step here). False if the id is not registered.
    bool removeOwnObject(ObjectId id) override;

    // Const-only access; mutation goes through the facade above.


private:

    // Shared box-feature registration/wiring logic for addBoxFeature and
    // restoreBoxFeature (single registration path, spec 13).
    void wireBoxFeature(BoxFeature& feature, ObjectId widthParameterId,
                       ObjectId heightParameterId, ObjectId depthParameterId,
                       ObjectId materialId);

    // Shared Pad registration/wiring for addPadFeature and restorePadFeature
    // (single registration path, spec 13).
    void wirePocketFeature(PocketFeature& feature, ObjectId baseFeatureId, ObjectId sketchId,
                           ObjectId depthParameterId, ObjectId materialId);
    void wireImportFeature(ImportFeature& feature, ObjectId materialId);
    void wireBooleanFeature(BooleanFeature& feature, ObjectId targetFeatureId,
                            ObjectId toolFeatureId, ObjectId materialId);
    void wireCurvePatternFeature(CurvePatternFeature& feature, ObjectId baseFeatureId,
                                 ObjectId pathSketchId, ObjectId countParameterId,
                                 ObjectId materialId);
    void wireShellFeature(ShellFeature& feature, ObjectId baseFeatureId,
                          ObjectId thicknessParameterId, ObjectId materialId);
    void wireDraftFeature(DraftFeature& feature, ObjectId baseFeatureId,
                          ObjectId angleParameterId, ObjectId materialId);
    void wireHoleFeature(HoleFeature& feature, ObjectId baseFeatureId, ObjectId sketchId,
                         ObjectId diameterParameterId, ObjectId depthParameterId,
                         ObjectId materialId);
    void wireSweepFeature(SweepFeature& feature, ObjectId profileSketchId, ObjectId pathSketchId,
                          ObjectId materialId);
    void wireLoftFeature(LoftFeature& feature, const std::vector<ObjectId>& sectionSketchIds,
                         ObjectId materialId);
    void wireRevolveFeature(RevolveFeature& feature, ObjectId sketchId,
                            ObjectId angleParameterId, ObjectId materialId);
    void wireTransformFeature(TransformFeature& feature, ObjectId baseFeatureId,
                              ObjectId frameId, ObjectId countParameterId,
                              ObjectId spacingParameterId, ObjectId materialId);
    void wireEdgeDressFeature(class EdgeDressFeature& feature, ObjectId baseFeatureId,
                              ObjectId sizeParameterId, ObjectId materialId);
    void wirePadFeature(PadFeature& feature, ObjectId sketchId, ObjectId lengthParameterId,
                       ObjectId materialId);

    // The duplicate-id rule at the door, for EVERY restore path (round 4,
    // R1R4-C1). Round 3 gave `restorePlaceholderFeature` a two-half check --
    // registry plus an all-bodies feature scan -- and its own comment named
    // the general fact: placeholders are unregistered, so a placeholder-held
    // id is invisible to `registry_.contains` and therefore defeated every
    // SIBLING guard too. The comment named it; nothing closed it. Restoring a
    // Pad onto a placeholder's id left two features carrying one ObjectId in
    // one Body, and the repair was worse than the disease -- `removeObject`
    // resolved the Pad through the registry, unregistered it and dropped its
    // graph node, then `Body::removeFeature` erased the FIRST match, the
    // placeholder, leaving the Pad as an unregistered, graph-less orphan that
    // saved and loaded cleanly as a healthy Pad. Silent divergence between
    // memory and file, ADR-M8-008's unconstructible state constructed through
    // the public facade alone.
    //
    // One helper, called before ANYTHING is stored, so a throw leaves no
    // residue. `who` is the caller's own name, because the loader mirrors
    // these strings and round-2/3 findings pin several of them word for word.






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

    // --- Expression plumbing (M11.2) ---------------------------------------
    // The names an expression reads, or an empty list when it does not parse.
    // Deliberately silent about parse failure: the callers that need the error
    // have already produced it, and the ones that do not (edge teardown for a
    // text that is being REPLACED) must not be blocked by it.
    static std::vector<std::string> expressionVariableNames(const std::string& text);
    // Resolve one name to a parameter. `ambiguous` is set when two parameters
    // share the name -- distinct from "not found", because the fix differs.
    const Parameter* findParameterByExpressionName(const std::string& name,
                                                   bool& ambiguous) const;
    // Drop the edges `text`'s references imply. No-op for names that no longer
    // resolve; a removed prerequisite has already taken its edge with it.
    // The parameter ids `text` reads. Names that do not resolve are skipped:
    // a removed prerequisite has already taken its edge with it.
    std::vector<ObjectId> expressionPrerequisites(const std::string& text) const;
    void detachExpressionEdges(ObjectId parameterId, const std::string& text);
    // "Width -> Height -> Width", for a cycle refusal message. Bounded by the
    // node count; returns an empty string when no path exists.
    std::string describeDependencyPath(ObjectId from, ObjectId to) const;

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

    // Undo internals (M9.1). `applyingHistory_` is the re-entrancy guard: undo
    // and redo drive the very facade methods that record, and without it the
    // first undo would push its own inverse and the stack would oscillate.
    // Writes a name with no validation and no undo record -- the shared tail
    // of renameObject and of undo, so a replayed rename cannot take a path
    // the original did not.




    // --- What this document owes DocumentBase (M23, ADR-M23-001) ------------
    //
    // Each of these is the Part half of a rule the base states once. They are
    // private and the base is a friend of nothing: it reaches them through the
    // virtuals it declared, so there is no second way in.
    void requireUnusedIdHook(ObjectId id, const char* who) const override;
    void forEachOwnNamed(const std::function<void(const NamedSlot&)>& visit) override;
    void applyOwnDelta(const UndoDelta& delta, bool forward) override;
    void onGraphDirtied() noexcept override { syncFeatureStatesFromGraph(); }
    void beforeRecomputePass() override { reconcileAllSketchParameterEdges(); }
    bool isNodeActive(ObjectId id) const noexcept override { return isFeatureActive(id); }

    void recordFeatureAdded(const Body& body, const Feature& feature);


    // Re-points the mass source at the body's chain TAIL -- the last solid
    // feature nothing else consumes -- or detaches it if the body has none.
    void rewireMassPropertiesToTail(const Body& body);



    ParameterManager parameters_;
    std::vector<std::unique_ptr<Body>> bodies_;

    std::vector<std::unique_ptr<Sketch>> sketches_;
    std::shared_ptr<Material> material_;
    MassProperties massProperties_;

    MassPropertiesNode massPropertiesNode_;
};

} // namespace paramcad
