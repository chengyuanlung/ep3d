#pragma once

#include "Core/Assembly/AssemblyStates.h"
#include "Core/Assembly/IAssemblySolver.h"
#include "Core/Assembly/Mate.h"
#include "Core/Assembly/Relation.h"
#include "Core/Assembly/Instance.h"
#include "Core/Document/DocumentBase.h"
#include "Core/Geometry/MathTypes.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

namespace paramcad {

// THE SECOND DOCUMENT TYPE (M23, ADR-M23-002).
//
// What an assembly holds is INSTANCES: appearances of parts, each somewhere.
// It holds no sketches, no features and no bodies, and that is not an
// omission -- an assembly does not model shape, it arranges shapes that were
// modelled elsewhere.
//
// Almost nothing here is new machinery. Identity, the dependency graph, undo,
// reference frames, connectors, naming and the recompute engine all come from
// DocumentBase, which M23 created for exactly this reason (ADR-M23-001): the
// plan's P3 says the cost of a second document type is paid once or paid for
// ever. What is left below is the part that really is about assemblies, and it
// is short enough to read in one sitting -- which is the evidence that P3 was
// honoured rather than a claim about it.
//
// NOT here yet, and named rather than half-built: mates (M24), degrees of
// freedom and interference (M25), sub-assemblies and exploded states (M26).
// An instance today is placed by setting its transform directly, which is what
// Onshape calls a fixed instance dragged by hand.
class AssemblyDocument final : public DocumentBase {
public:
    explicit AssemblyDocument(std::string name);
    // Restore constructor (deserialization): keeps the persisted document id.
    AssemblyDocument(ObjectId id, std::string name);

    DocumentType type() const noexcept override { return DocumentType::Assembly; }

    // --- Instances -----------------------------------------------------------
    //
    // `sourcePath` names an .ep3d file and `bodyName` a body inside it. An
    // empty body name means "the only one", and a file with several bodies and
    // no name given is REFUSED at recompute time with the names it does have --
    // taking the first would make an instance mean something different after
    // the part gained a body, which is position-as-identity (ADR-M4-004).
    //
    // Every instance gets its own ReferenceFrame, parented to Origin, named
    // "<instance> origin". That frame IS the placement: there is no second
    // transform anywhere, so an instance cannot be in two places.
    Instance& addInstance(std::string name, std::string sourcePath,
                              std::string bodyName = {});
    // Restore path: the frame already exists and is passed in, because the
    // loader restores frames before instances and re-deriving one here would
    // give the file's frame and the instance's frame different ids.
    Instance& restoreInstance(ObjectId id, std::string name, ComputeState state,
                                  std::string sourcePath, std::string bodyName,
                                  ObjectId frameId);

    std::vector<const Instance*> instances() const;
    const Instance* findInstance(ObjectId id) const noexcept;
    const Instance* findInstanceNamed(const std::string& name) const noexcept;

    // Moves an instance. Goes through the frame, so it is undoable, it dirties
    // the instance through an ordinary graph edge, and a sub-assembly's
    // children will follow it in M26 without anything here changing.
    // False if the id is not an instance.
    bool setInstanceTransform(ObjectId instanceId, const Transform3D& placement);
    // Where the instance sits relative to its parent, and in the assembly's
    // world. Identity for an unknown id, exactly as worldTransform is.
    Transform3D instanceTransform(ObjectId instanceId) const noexcept;
    Transform3D instanceWorldTransform(ObjectId instanceId) const noexcept;

    // --- Patterns (M26, ADR-M26-003) -----------------------------------------
    //
    // `count` copies of `instanceId`, each `step` further along than the last.
    //
    // A COPY'S FRAME IS A CHILD OF THE ORIGINAL'S, which is not an
    // implementation detail: it is what makes the pattern parametric. Move the
    // original and the copies follow, because the frame hierarchy already
    // composes -- nothing here watches anything.
    //
    // Returns the ids of the copies (count - 1 of them; the original is the
    // first of the row and is not touched). Throws if the id is not an
    // instance or the count is below one.
    //
    // NOT A STORED FEATURE. Deleting the pattern means deleting the copies,
    // which are ordinary instances -- and that is the honest shape while an
    // assembly has no feature list to put a pattern in. Said out loud rather
    // than implied: editing the count afterwards means deleting and redoing.
    std::vector<ObjectId> addInstancePattern(ObjectId instanceId, int count,
                                             const Vec3& step);

    // --- Named positions (M26, roadmap §49) ----------------------------------
    //
    // `capture` reads the current pose -- every mate's freedoms, and where the
    // instances no mate places are sitting. `apply` puts it back, as ONE undo
    // step, because a pose is one thing a user chose.
    NamedPosition& captureNamedPosition(std::string name);
    NamedPosition& restoreNamedPosition(ObjectId id, std::string name,
                                        std::vector<NamedPosition::MateSetting> mates,
                                        std::vector<NamedPosition::LooseSetting> loose);
    bool applyNamedPosition(ObjectId positionId);
    std::vector<const NamedPosition*> namedPositions() const;
    const NamedPosition* findNamedPosition(ObjectId id) const noexcept;
    const NamedPosition* findNamedPositionNamed(const std::string& name) const noexcept;

    // --- Exploded views (M26, roadmap §49) -----------------------------------
    //
    // An ordered list of displacements FOR A PICTURE. It never changes the
    // model: `explodedWorldTransform` composes the view on top of where the
    // assembly actually put the instance, and asking with no view gives the
    // assembly's own answer.
    ExplodeView& addExplodeView(std::string name);
    ExplodeView& restoreExplodeView(ObjectId id, std::string name,
                                    std::vector<ExplodeStep> steps, std::size_t previewCut);
    // Appends a step. Throws if the instance is not one of this assembly's.
    bool addExplodeStep(ObjectId viewId, std::string stepName, ObjectId instanceId,
                        const Vec3& offset);
    // Moves a step to a new position in the list, because §49 says steps can be
    // reordered and an explosion whose steps cannot be reordered is a list that
    // has to be retyped to fix.
    bool moveExplodeStep(ObjectId viewId, std::size_t from, std::size_t to);
    bool removeExplodeStep(ObjectId viewId, std::size_t index);
    // How many steps to show. EvaluationCut::kAll is the finished explosion.
    bool setExplodePreview(ObjectId viewId, std::size_t stepsShown);

    std::vector<const ExplodeView*> explodeViews() const;
    const ExplodeView* findExplodeView(ObjectId id) const noexcept;
    const ExplodeView* findExplodeViewNamed(const std::string& name) const noexcept;

    // Where an instance appears when `viewId` is being shown. Identity view
    // (kInvalidObjectId) gives instanceWorldTransform unchanged, which is the
    // evidence that an explosion is a picture and not a move.
    Transform3D explodedWorldTransform(ObjectId viewId, ObjectId instanceId) const noexcept;

    // --- Relations (M31, roadmap §20.5) --------------------------------------
    //
    // A relation couples two freedoms a mate solve would otherwise be free to
    // choose independently: two gears turn together, a rack advances as its
    // pinion turns, a screw travels as it turns.
    //
    // AFTER MATES, ALWAYS. A relation's input is a MATE (§20.5), so one cannot
    // exist before the mates it couples -- which is also why it holds MateIds
    // rather than instances.
    //
    // Refused, by name, when the freedoms do not suit the type: a gear needs
    // two rotations, a screw needs one mate's rotation and its own travel. The
    // rule lives in WhyRelationIsRefused so the document, the loader and the
    // UI cannot disagree about it.
    //
    // THROWS on a refusal, matching addInstancePattern: these are programming
    // errors from a UI that should have disabled the command, not user input
    // arriving at the model.
    Relation& addRelation(std::string name, RelationType type, CoupledFreedom driver,
                          CoupledFreedom driven, double ratio, bool reversed = false);
    Relation& restoreRelation(ObjectId id, std::string name, RelationType type,
                              CoupledFreedom driver, CoupledFreedom driven, double ratio,
                              bool reversed);
    std::vector<const Relation*> relations() const;
    const Relation* findRelation(ObjectId id) const noexcept;
    const Relation* findRelationNamed(const std::string& name) const noexcept;
    bool setRelationRatio(ObjectId relationId, double ratio);
    bool setRelationReversed(ObjectId relationId, bool reversed);

    // The relation that WRITES this freedom, or nullptr when the solve or the
    // user still decides it.
    //
    // PUBLIC because the shell has to ask before it offers to drive a mate: a
    // freedom a relation writes is not the user's to set, and "Driven to 0.5"
    // over a part that did not move is the worst way to find that out.
    const Relation* relationDriving(ObjectId mateId, MateComponent component) const noexcept;

    // Why this pair of freedoms cannot be coupled by this type, or empty when
    // it can. Checks the mates EXIST and actually leave those freedoms free,
    // which the type-only rule cannot know.
    std::string whyRelationIsRefused(RelationType type, const CoupledFreedom& driver,
                                     const CoupledFreedom& driven) const;

    // WHAT A MATE'S VALUES ACTUALLY ARE once relations have had their say.
    //
    // A relation-driven freedom is no longer the mate's own number: whatever
    // the last solve or the last edit left in it, the relation decides it.
    // The solve uses this same code path, so the panel and the geometry
    // cannot disagree about what a coupled freedom currently is.
    MateValues valuesAfterRelations(ObjectId mateId) const;

    // --- Grounding (M24) -----------------------------------------------------
    //
    // A mate says where something is RELATIVE to something else, so a chain of
    // them has to start somewhere that is not relative to anything. That is
    // what grounding is: this instance stays where it was put, and everything
    // mated to it follows.
    //
    // Nothing is grounded by default, and an assembly whose mates reach no
    // ground is REFUSED with the names -- because "somewhere" is not an answer
    // and putting the first instance in the list there would make the answer
    // depend on the order things were typed.
    bool setInstanceGrounded(ObjectId instanceId, bool grounded);
    bool isInstanceGrounded(ObjectId instanceId) const noexcept;
    // The restore-path twin: grounds without recording an undo step.
    bool restoreInstanceGrounded(ObjectId instanceId);

    // --- Mates (M24, ADR-M24-002) --------------------------------------------
    //
    // Each side is an instance and a connector NAME on the part it brings in.
    // `value` is the mate's one remaining freedom -- radians for a revolute,
    // millimetres for a slider -- and must be zero for a Fastened mate, which
    // has no freedom to spend it on.
    //
    // Throws when an id is not an instance, when both sides are the SAME
    // instance (a thing cannot be mated to itself), or when a Fastened mate is
    // handed a value.
    Mate& addMate(std::string name, MateType type, ObjectId leadingInstanceId,
                  std::string leadingConnector, ObjectId followingInstanceId,
                  std::string followingConnector, double value = 0.0);
    Mate& restoreMate(ObjectId id, std::string name, MateType type, ObjectId leadingInstanceId,
                      std::string leadingConnector, ObjectId followingInstanceId,
                      std::string followingConnector, double value);
    // The full form: a value per component, for mates that leave more than one
    // freedom (a cylindrical turns AND slides).
    Mate& addMateWithValues(std::string name, MateType type, ObjectId leadingInstanceId,
                            std::string leadingConnector, ObjectId followingInstanceId,
                            std::string followingConnector, MateValues values);
    Mate& restoreMateWithValues(ObjectId id, std::string name, MateType type,
                                ObjectId leadingInstanceId, std::string leadingConnector,
                                ObjectId followingInstanceId, std::string followingConnector,
                                MateValues values, bool driven,
                                const std::array<Mate::Limit, kMateComponentCount>& limits);

    std::vector<const Mate*> mates() const;
    const Mate* findMate(ObjectId id) const noexcept;
    const Mate* findMateNamed(const std::string& name) const noexcept;

    // Drives the mate's one freedom. This is what "turn the hinge" means: it
    // is an ordinary undoable edit, and the next rebuild moves everything
    // downstream of it. False if the id is not a mate; refused for Fastened.
    bool setMateValue(ObjectId mateId, double value);
    // The same, for a named component -- what a cylindrical mate needs, since
    // "the value" is ambiguous when a mate leaves two freedoms.
    //
    // `clampedTo` is filled with what the value actually became: a limit stops
    // the motion rather than refusing it (roadmap §22), and a stop nobody is
    // told about is a control that appears to be broken.
    bool setMateComponentValue(ObjectId mateId, MateComponent component, double value,
                               double* clampedTo = nullptr);

    // --- Motion limits (M25, roadmap §22) ------------------------------------
    //
    // Refused for a component this kind of mate does not free: a limit on
    // something that cannot move is a control with nothing behind it.
    bool setMateLimit(ObjectId mateId, MateComponent component, double minimum, double maximum);
    bool clearMateLimit(ObjectId mateId, MateComponent component);

    // --- Driving (M25) --------------------------------------------------------
    //
    // A driven mate holds its values through a closed-loop solve; an undriven
    // one is what the solve is allowed to move. In an assembly with no loops
    // this changes nothing.
    bool setMateDriven(ObjectId mateId, bool driven);

    // WHERE A MATE CONNECTOR ACTUALLY IS, in the assembly (M24).
    //
    // The instance's placement composed with the connector's place on the
    // part. Composed on demand, never stored -- which is what makes "the two
    // connectors are the same place" a thing that can be MEASURED rather than
    // assumed, and that measurement is the half of the hinge gate a picture
    // cannot check.
    //
    // `found` is set false when the instance or the connector name does not
    // resolve, because identity is not a place and returning one would be a
    // lie a caller cannot distinguish from an answer.
    Transform3D mateConnectorWorldTransform(ObjectId instanceId, const std::string& connectorName,
                                            bool* found = nullptr) const noexcept;

    // --- What the solve concluded (M24, ADR-M24-004) --------------------------
    //
    // Filled by the last recompute. Empty message and true means the mates were
    // satisfiable and every instance was placed; false names what stopped it.
    struct MateSolveReport {
        bool ok = true;
        std::string message;
        // Per instance, the degrees of freedom the mates LEFT it (roadmap
        // §20.3 requires this be readable per instance, not as one number for
        // the assembly: "this assembly is under-constrained" is not something
        // a user can act on).
        struct InstanceFreedom {
            ObjectId instanceId = kInvalidObjectId;
            int rotational = 0;
            int translational = 0;
            std::string describedBy; // the mate that decided it, or "ground"
        };
        std::vector<InstanceFreedom> freedoms;

        // M25. How many freedoms the CLOSED LOOPS leave, measured from the
        // rank of the solve's Jacobian rather than counted from unknowns minus
        // equations -- a planar four-bar writes three equations that are
        // identically zero at every configuration, and counting would call it
        // over-constrained while it turns perfectly well.
        //
        // Reported once, for the mechanism, because inside a loop the freedom
        // does not belong to any one instance: a four-bar whose three moving
        // links each "have one rotation" reads as three when the linkage has
        // one.
        int mechanismDegreesOfFreedom = 0;
        int iterations = 0;
    };

    // --- Interference (M25, roadmap §23) -------------------------------------
    //
    // Kept SEPARATE from mates, because §23 is right that a perfectly legal set
    // of mates can still drive two parts through each other. Broad phase on
    // bounding boxes, then a precise kernel intersection on what survives.
    struct Interference {
        ObjectId firstInstanceId = kInvalidObjectId;
        ObjectId secondInstanceId = kInvalidObjectId;
        double volumeMm3 = 0.0;
    };
    // Every overlapping pair, in instance order. Requires a kernel and a built
    // assembly; an instance that has not been built is skipped rather than
    // reported as clear, and `message` says so.
    struct InterferenceReport {
        bool ok = true;
        std::string message;
        std::vector<Interference> overlaps;
    };
    InterferenceReport checkInterference() const;

    // --- IS THAT ANSWER STILL TRUE? (M46) ------------------------------------
    //
    // WHY THIS IS HERE AND CONTACT SOLVING IS NOT.
    //
    // The obvious next thing is a solver that will not let two parts occupy
    // the same space -- and it is not a bigger residual vector, it is a
    // different algorithm. Everything the mate solve does drives residuals to
    // ZERO; a contact is `gap >= 0`, an INEQUALITY, and least squares cannot
    // say that. It needs an active set: guess which contacts are touching,
    // solve those as equalities, check the rest, repeat -- and the guess can
    // legitimately change mid-drag, which is how such solvers chatter and stop
    // converging.
    //
    // Worse, the residual would need GEOMETRY. Today `evaluate` composes
    // transforms and never touches the kernel. A contact residual is the
    // distance between two solids at a configuration, and measureInterference
    // returns a VOLUME -- which is zero for every pair that is not already
    // overlapping, so it has no gradient to descend. A signed distance query
    // is what would be needed, this kernel does not have one, and a B-rep
    // distance per iteration per drag frame is milliseconds where the mate
    // solve costs microseconds.
    //
    // So what is done instead is the small honest thing. The clash check
    // already exists and is EXACT. What it lacked was any way to know it was
    // out of date: a user checked, got "clear", then dragged a link through a
    // wall, and the answer on the screen still said clear. The same shape as a
    // drawing view that is behind its model (M32) -- and the same fix: the
    // answer says WHEN it was true, and anything that moves the assembly makes
    // it stale.
    //
    // A check on every drag was the other option and was rejected: an
    // all-pairs B-rep intersection per mouse move is a performance cliff, and
    // a threshold for "small enough to check" would be a number nobody could
    // defend.
    bool isInterferenceStale() const noexcept { return interferenceStale_; }
    // The last answer, and whether it is still about this assembly. Empty
    // overlaps with `stale` true means "nobody has looked since it moved", not
    // "clear".
    const InterferenceReport& lastInterference() const noexcept { return lastInterference_; }
    // Runs the check and remembers it. checkInterference stays const and
    // stateless for callers that only want an answer.
    const InterferenceReport& recheckInterference();
    const MateSolveReport& mateSolveReport() const noexcept { return solveReport_; }

    // The solver that closes loops. Injected the same way and for the same
    // reason as the kernel and the sketch solver (ADR-M3-003): nullptr is a
    // normal, tested state, and an assembly with no loops never needs one.
    void setAssemblySolver(IAssemblySolver* solver) noexcept { assemblySolver_ = solver; }
    IAssemblySolver* assemblySolver() const noexcept { return assemblySolver_; }
    // The chain of assembly files open above this one, so a sub-assembly that
    // contains its own parent is refused rather than recursing (M26).
    void setSourceChain(std::vector<std::string> chain) { sourceChain_ = std::move(chain); }

    DocumentRecomputeReport recompute() override;

    bool removeOwnObject(ObjectId id) override;

protected:
    void requireUnusedIdHook(ObjectId id, const char* who) const override;
    void forEachOwnNamed(const std::function<void(const NamedSlot&)>& visit) override;
    void applyOwnDelta(const UndoDelta& delta, bool forward) override;
    const std::vector<std::string>* sourceChain() const override { return &sourceChain_; }
    IAssemblySolver* assemblySolverForNodes() const override { return assemblySolver_; }

private:
    Instance* findInstanceForEdit(ObjectId id) noexcept;
    // The frame an instance is placed by, created and wired in one place so
    // add and restore cannot disagree about what an instance's frame is.
    void wireInstance(Instance& instance);

    Mate* findMateForEdit(ObjectId id) noexcept;
    NamedPosition* findNamedPositionForEdit(ObjectId id) noexcept;
    ExplodeView* findExplodeViewForEdit(ObjectId id) noexcept;
    // The rules a mate has to pass before it exists at all. Shared by add and
    // restore so the two doors cannot come to disagree about what a legal mate
    // is -- which is how a document that saves cleanly stops loading.
    void requireMatable(ObjectId leadingInstanceId, ObjectId followingInstanceId, MateType type,
                        const MateValues& values, const char* who) const;
    // WHICH MATE PLACES WHICH INSTANCE, and which mates are left over.
    //
    // Built once per solve and reused for every probe a closed-loop search
    // makes: the shape of the walk does not change when the angles do.
    struct MateForest {
        struct Step {
            ObjectId mateId = kInvalidObjectId;
            ObjectId from = kInvalidObjectId;
            ObjectId to = kInvalidObjectId;
        };
        std::vector<ObjectId> roots;
        std::vector<Step> steps;
        std::vector<ObjectId> loopClosers;
        std::unordered_set<ObjectId> reached;
        std::string message;
    };
    bool buildMateForest(MateForest& forest) const;
    bool placeThroughForest(const MateForest& forest, const std::vector<MateValues>& mateValues,
                            std::unordered_map<ObjectId, Transform3D>& placed,
                            std::string* whyNot) const;
    bool loopResiduals(const MateForest& forest, const std::vector<MateValues>& mateValues,
                       const std::unordered_map<ObjectId, Transform3D>& placed, double* out,
                       std::size_t* count) const;
    // Position in mates_, which is how the value vectors are indexed.
    std::size_t mateIndex(ObjectId mateId) const noexcept;

    // Places every instance the mates reach, starting from the grounded ones.
    // Returns false and fills `solveReport_` when it cannot.
    bool solveMates();

    std::vector<std::unique_ptr<Instance>> instances_;
    std::vector<std::unique_ptr<Mate>> mates_;
    std::vector<std::unique_ptr<Relation>> relations_;

    // Rewrites every RELATION-DRIVEN component of `values` from its driver.
    //
    // Called wherever mate values are about to place something, so a driven
    // freedom is never read from the mate's own stored number -- that number
    // is whatever the last solve or the last user edit left there, and the
    // relation is what decides it now.
    void applyRelations(std::vector<MateValues>& values) const;
    // Whether (mateId, component) is written by some relation, and therefore
    // is NOT an unknown the loop solver may choose.
    bool isDrivenByRelation(ObjectId mateId, std::size_t component) const noexcept;
    std::vector<std::unique_ptr<NamedPosition>> namedPositions_;
    std::vector<std::unique_ptr<ExplodeView>> explodeViews_;
    std::vector<ObjectId> groundedInstances_;
    MateSolveReport solveReport_;
    IAssemblySolver* assemblySolver_ = nullptr;
    // M46. The last clash answer and whether the assembly has moved since.
    // STALE BY DEFAULT: a freshly opened assembly has not been checked, and
    // "not checked" must never read as "clear".
    InterferenceReport lastInterference_;
    bool interferenceStale_ = true;
    std::vector<std::string> sourceChain_;
};

} // namespace paramcad
