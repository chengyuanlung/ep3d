#pragma once

#include "Core/Connector/Connector.h"
#include "Core/Dependency/DependencyGraph.h"
#include "Core/Document/CadDocument.h"
#include "Core/Document/ObjectRegistry.h"
#include "Core/Expression/ExpressionTypes.h"
#include "Core/Geometry/MathTypes.h"
#include "Core/Recompute/DocumentRecomputeEngine.h"
#include "Core/Reference/ReferenceFrame.h"
#include "Core/Undo/UndoRecord.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace paramcad {

class IRecomputable;
class IGeometryKernel;
class ISketchSolver;
class IAssemblySolver;

// EVERYTHING A DOCUMENT IS, REGARDLESS OF WHAT IT HOLDS (M23, ADR-M23-001).
//
// This class exists because M23 adds the SECOND document type, and the plan's
// P3 says the cost of that is paid once or it is paid for ever: an Assembly
// that copied PartDocument's object registry, dependency graph, undo stack,
// id rules and frame hierarchy would be five more pairs of things that must
// agree -- which is the single defect class this project keeps finding
// (ADR-M17-*, ADR-M21-001). Drawing is the third document type, and it will
// inherit this rather than copy either of the two.
//
// What is here is exactly what is true of a document as such:
//
//   * IDENTITY -- an ObjectRegistry, and the one rule that an id is used once
//     (`requireUnusedId`);
//   * DEPENDENCE -- a DependencyGraph, and the single edge-direction
//     convention stated below;
//   * HISTORY -- the undo/redo stacks, transactions, and the re-entrancy
//     guard that keeps a replayed edit from recording its own inverse;
//   * PLACE -- reference frames and connectors. A frame hierarchy is not a
//     part concept: an assembly instance's placement IS a frame, and a mate
//     connector IS a frame plus meaning (ADR-M10-004), so putting frames here
//     is what lets M24's mates be built on machinery that already works;
//   * NAMES -- one rename path, one uniqueness rule.
//
// What is NOT here is anything about WHAT the document holds. Parameters,
// sketches, bodies and features are a Part's business; instances are an
// Assembly's. Where the generic machinery has to reach into that -- the id
// rule has to see a Part's unregistered placeholders, a rename has to reach a
// Part's features -- it does so through a named virtual hook, and each hook's
// declaration says what a subclass owes it.
//
// DEPENDENCY DIRECTION (single rule, ADR-007/ADR-012): an edge points
// prerequisite -> dependent; "A -> B" means B depends on A and dirtiness flows
// from A downstream to B. addDependency(dependent, prerequisite) reads
// "dependent consumes prerequisite" -- the facade mirrors the DependencyGraph
// signature exactly so the project has one parameter order everywhere.
class DocumentBase : public CadDocument {
public:
    ~DocumentBase() override = default;

    // engine_-style self-reference lives in the subclasses, but the frames and
    // connectors here are held by unique_ptr and handed out as raw pointers
    // registered in registry_, so copying a document would leave two
    // registries pointing at one set of objects.
    DocumentBase(const DocumentBase&) = delete;
    DocumentBase& operator=(const DocumentBase&) = delete;
    DocumentBase(DocumentBase&&) = delete;
    DocumentBase& operator=(DocumentBase&&) = delete;

    // --- Reference frames as first-class objects (M10, ADR-M10-001) ---------
    //
    // Registered, graph-participating, undoable. `parentFrameId` may be
    // kInvalidObjectId for a root frame.
    //
    // Throws std::runtime_error when the parent is not a frame of this
    // document, or when the parent chain would CYCLE -- refused at the door
    // rather than discovered by an unbounded walk at recompute time, which is
    // the cost M9.1 measured on itself.
    ReferenceFrame& addFrame(std::string name, ObjectId parentFrameId = kInvalidObjectId);
    ReferenceFrame& restoreFrame(ObjectId id, std::string name, ObjectId parentFrameId,
                                 const Transform3D& localTransform);

    std::vector<const ReferenceFrame*> frames() const;
    const ReferenceFrame* findFrame(ObjectId id) const noexcept;

    // Sets a frame's transform relative to its parent. Dirties the frame,
    // which dirties everything downstream of it through the ordinary M2
    // machinery. False if the id is not a frame.
    bool setFrameTransform(ObjectId frameId, const Transform3D& transform);
    // Re-parents. Same refusals as addFrame; false if either id is wrong.
    bool setFrameParent(ObjectId frameId, ObjectId parentFrameId);
    // The restore-path twin of setFrameParent: records no undo step.
    bool restoreFrameParent(ObjectId frameId, ObjectId parentFrameId);

    // The frame's transform in DOCUMENT-LOCAL world coordinates, COMPOSED from
    // the parent chain (ADR-M10-002). Never stored: a cached world transform is
    // two truths that disagree the moment a parent moves.
    //
    // Identity for an unknown id, which is the same answer a document with no
    // frames gives, so a caller that never uses frames is unaffected.
    Transform3D worldTransform(ObjectId frameId) const noexcept;

    // --- Connectors as first-class objects (M10.3, ADR-M10-004) -------------
    //
    // Registered and resolvable, whichever route created them (§18.1). A
    // connector holds no geometry: it references a frame, and the frame answers
    // where it is -- `connectorWorldTransform` is the composition of the two.
    //
    // Throws when `frameId` is not a frame of this document: a connector on
    // nothing is a mate anchor that cannot be resolved, which is A03's failure
    // mode rather than a recoverable state.
    Connector& addConnector(std::string name, ConnectorRole role, ObjectId frameId,
                            ConnectorOwner owner = ConnectorOwner::PartDefinition);
    Connector& restoreConnector(ObjectId id, std::string name, ConnectorRole role,
                                ObjectId frameId, ConnectorOwner owner);
    std::vector<const Connector*> connectors() const;
    const Connector* findConnector(ObjectId id) const noexcept;
    // The connector's frame, composed to world. Identity for an unknown id.
    Transform3D connectorWorldTransform(ObjectId connectorId) const noexcept;

    // --- Naming (M17.16, ADR-M17-039; generic since M23) --------------------
    //
    // Works for anything the tree shows a name for. The base answers for the
    // objects it owns -- frames and connectors -- and asks the subclass for
    // the rest, so a Part's features and an Assembly's instances are renamed
    // by the same call, with the same trimming and the same uniqueness rule.
    struct RenameResult {
        bool ok = false;
        std::string message;
    };
    RenameResult renameObject(ObjectId id, std::string name);
    // The object's current name, or empty when nothing in this document
    // carries that id or the thing that does has no name.
    std::string objectName(ObjectId id) const;

    // A name nothing in this document is using, made from `wanted` by adding
    // " 2", " 3", ... until it is free.
    //
    // PUBLIC BECAUSE THE UI NEEDS IT (M31). Every facade that takes a name
    // throws on a duplicate -- it treats one as a caller that should have
    // known -- so a shell inventing "Gear" has to be able to ask. The shells
    // that instead checked one KIND's names (findMateNamed, and the sketch
    // panel before it) were each one rename away from throwing at the user,
    // because uniqueness here is across the whole document and always was.
    std::string unusedNameLike(const std::string& wanted) const;

    // --- Undo (M9.1, ADR-M9-001) --------------------------------------------
    void beginTransaction(std::string label);
    bool commitTransaction();
    // Undoes everything recorded since `beginTransaction` and records NOTHING.
    bool abortTransaction();
    bool undo();
    bool redo();
    bool canUndo() const noexcept { return !undoStack_.empty(); }
    bool canRedo() const noexcept { return !redoStack_.empty(); }
    std::size_t undoDepth() const noexcept { return undoStack_.size(); }
    std::size_t redoDepth() const noexcept { return redoStack_.size(); }
    // True between beginTransaction and commit/abort. Read by callers that
    // must not open a nested one.
    bool isTransactionOpen() const noexcept { return openTransaction_.has_value(); }
    std::string nextUndoLabel() const;
    std::string nextRedoLabel() const;

    // --- Graph and registry --------------------------------------------------
    GraphResult addRecomputableNode(IRecomputable& object);
    GraphResult addDependency(ObjectId dependent, ObjectId prerequisite);
    GraphResult removeDependency(ObjectId dependent, ObjectId prerequisite);
    virtual bool markDirty(ObjectId id);

    // Unhooks graph -> registry -> owner, in that order, so no dangling
    // reference is reachable through a public path. Subclasses own the last
    // step for the objects they hold; frames and connectors are unhooked here.
    virtual bool removeObject(ObjectId id) = 0;
    // The restore-path twin: removes recording NO undo step. Used by every
    // loader to drop the constructor's auto-created Origin when the file
    // supplies its own frames -- without it, a load arrives with one undo step
    // that would delete the document's Origin (ADR-M9-001; caught by gate H
    // for parts, and by M23_SER_001 for assemblies, which is twice the same
    // defect and the reason this now lives in one place).
    bool restoreRemoveObject(ObjectId id);

    // --- The tools a document computes WITH (ADR-M3-003) --------------------
    //
    // Non-owning, injected, and nullptr is a normal tested state rather than a
    // precondition. Here rather than in PartDocument since M23: an assembly
    // instance loads a part and rebuilds it, so it needs both -- and two
    // documents each holding their own pointer pair is two places for
    // "configured" to mean different things.
    virtual void setGeometryKernel(IGeometryKernel* kernel) noexcept { kernel_ = kernel; }
    IGeometryKernel* geometryKernel() const noexcept { return kernel_; }
    virtual void setSketchSolver(ISketchSolver* solver) noexcept { sketchSolver_ = solver; }
    ISketchSolver* sketchSolver() const noexcept { return sketchSolver_; }

    // --- Recompute ----------------------------------------------------------
    //
    // ONE engine, driving both document types. What differs between them is
    // asked for by name through the three virtuals below, rather than by the
    // engine knowing which kind of document it holds.
    virtual DocumentRecomputeReport recompute();
    virtual DocumentRecomputeReport recomputeFrom(ObjectId id);

    const ObjectRegistry& objectRegistry() const noexcept { return registry_; }
    const DependencyGraph& dependencyGraph() const noexcept { return graph_; }

protected:
    explicit DocumentBase(std::string name);
    DocumentBase(ObjectId id, std::string name);

    // --- What a subclass owes this class ------------------------------------
    //
    // Each of these exists because a generic rule has to see objects only the
    // subclass knows about. They are not extension points to be used freely:
    // a subclass that leaves one empty is saying "this document has nothing of
    // that kind", and that claim is checked by the tests that pin each rule.

    // Ids the registry cannot see. PlaceholderFeature is deliberately never
    // registered (ADR-009 D4), so without a hook a placeholder-held id passes
    // the registry check and collides anyway. Throws, like the base's own
    // halves do.
    virtual void requireUnusedIdHook(ObjectId id, const char* who) const { (void)id; (void)who; }

    // --- Named objects: ONE list, three questions ---------------------------
    //
    // "What is this called", "write this name onto it" and "is this name
    // taken" are three readings of the same set. They used to be three
    // hand-kept walks per document type, which is this project's signature
    // defect wearing its plainest clothes: M31 shipped a relation that was in
    // NONE of the assembly's three, so a relation could take a name a mate
    // already had and then could not be renamed to anything.
    //
    // A subclass now says what it names ONCE. The three answers are derived
    // here, so a kind that is visited is answerable in all three, and a kind
    // that is not is answerable in none -- which is a visible hole rather than
    // a silent disagreement.
    struct NamedSlot {
        ObjectId id{kInvalidObjectId};
        std::string name;
        // Writes the new name where it belongs. NOT always a plain setter: an
        // assembly instance's placement frame is named after it and follows,
        // and that rule belongs with the instance rather than in a second
        // switch somewhere else.
        std::function<void(const std::string&)> rename;
    };

    // Visits every object this subclass names, in document order.
    //
    // NON-CONST because a slot carries a writer. The const readers below walk
    // through one documented const_cast rather than making the subclass keep a
    // second, read-only copy of the list -- which is the very duplication this
    // exists to remove.
    virtual void forEachOwnNamed(const std::function<void(const NamedSlot&)>& visit) {
        (void)visit;
    }

    // Apply an undo delta the base does not recognise. Reached only after the
    // base has tried its own; a subclass that meets a delta it does not know
    // should throw rather than return, because a silently skipped delta is an
    // undo that half-happened.
    virtual void applyOwnDelta(const UndoDelta& delta, bool forward) = 0;

    // The graph was just dirtied by base machinery -- a frame moved or was
    // re-parented -- and a subclass may cache state the graph is the truth
    // about. A Part demotes any Feature whose cached ComputeState now claims
    // Valid while the graph disagrees; an Assembly has no such cache.
    virtual void onGraphDirtied() noexcept {}

    // --- What the recompute engine asks a document ---------------------------

    // Run before every pass. A Part reconciles its Parameter -> Sketch edges
    // here, because a raw `Sketch&` can add a constraint without any facade
    // seeing it; an Assembly has no such back door and does nothing.
    virtual void beforeRecomputePass() {}

    // The chain of assembly files open above this document, so an assembly
    // that instances itself is refused rather than recursing (M26). Null for
    // a Part, which cannot contain an instance of anything.
    virtual const std::vector<std::string>* sourceChain() const { return nullptr; }
    // The solver a node may need for a nested mechanism. Null for a Part.
    virtual IAssemblySolver* assemblySolverForNodes() const { return nullptr; }

    // False for a node the pass must SKIP without failing it -- a Part's
    // rolled-back or suppressed features (M9.3/M9.4). Asked of the document
    // rather than baked into the graph, because the graph is generic and knows
    // nothing about bodies or feature order.
    virtual bool isNodeActive(ObjectId id) const noexcept { (void)id; return true; }

    // The value an expression's `#name` reads. A Part resolves it against its
    // Parameters; an Assembly has none yet, so nothing resolves and every
    // expression in one fails loudly rather than silently reading zero.
    virtual std::optional<Quantity> resolveExpressionVariable(std::string_view name) const {
        (void)name;
        return std::nullopt;
    }

    // --- The machinery itself ------------------------------------------------

    // Creates the Origin frame during construction, recording nothing.
    void createOriginFrame();

    // The duplicate-id rule at the door, for EVERY restore path. Checks the
    // document's own id, then the registry, then asks the subclass.
    void requireUnusedId(ObjectId id, const char* who) const;
    // A mate names a connector by NAME, so two with one name make a mate
    // mean whichever comes first. Throws, like the id rule does.
    void requireUnusedConnectorName(const std::string& name, const char* who) const;

    // Refuses a parent that is not a frame, or that would close a loop (M10).
    // Pass kInvalidObjectId as `frameId` when the frame does not exist yet --
    // a brand-new frame cannot be its own ancestor.
    void requireAcyclicParent(ObjectId frameId, ObjectId parentFrameId) const;

    // Writes a name with no validation and no undo record -- the shared tail
    // of renameObject and of undo, so a replayed rename cannot take a path
    // the original did not.
    void applyName(ObjectId id, const std::string& name);
    // Is `name` taken by anything in this document other than `except`?
    bool nameIsTaken(const std::string& name, ObjectId except) const;

    // The const half of the walk. One place, so the const_cast that makes a
    // mutable visitor answer a const question is written down once.
    void forEachOwnNamedConst(const std::function<void(const NamedSlot&)>& visit) const;

    void recordDelta(UndoDelta delta, std::string label);
    void clearHistory(const char* becauseOfWhat) noexcept;
    // Applies one delta: the base's own kinds, then the subclass's.
    void applyDelta(const UndoDelta& delta, bool forward);
    // True when `delta` was one of the base's own. Split out so a subclass's
    // removeObject can record a frame or connector deletion through the same
    // path that replays it.
    bool applyBaseDelta(const UndoDelta& delta, bool forward);

    // The two halves of removing a frame or a connector, for subclasses whose
    // removeObject has to interleave them with its own work. `recordBaseRemoval`
    // runs BEFORE anything is unhooked (so a refusal leaves the document
    // unchanged); `eraseBaseOwned` runs after the graph and registry are clean.
    // Both are no-ops for a handle the base does not own.
    // Moves every child of `frameId` onto ITS parent, keeping each one's world
    // transform unchanged. Run when a frame is about to be removed -- see the
    // definition for why a dangling parent link is worse than a dangling
    // reference anywhere else here.
    void liftChildFramesOf(ObjectId frameId);

    void recordBaseRemoval(const ObjectRegistry::ObjectRef& handle, ObjectId id);
    bool eraseBaseOwned(const ObjectRegistry::ObjectRef& handle, ObjectId id);

    // True while undo or redo is driving the facade. The re-entrancy guard:
    // without it the first undo would push its own inverse and the stack would
    // oscillate.
    bool applyingHistory() const noexcept { return applyingHistory_; }

    friend class DocumentRecomputeEngine; // the engine drives graph_/registry_

    std::vector<UndoRecord> undoStack_;
    std::vector<UndoRecord> redoStack_;
    std::optional<UndoRecord> openTransaction_;
    bool applyingHistory_ = false;

    std::vector<std::unique_ptr<ReferenceFrame>> frames_;
    std::vector<std::unique_ptr<Connector>> connectors_;
    ObjectRegistry registry_;
    DependencyGraph graph_;
    IGeometryKernel* kernel_ = nullptr;
    ISketchSolver* sketchSolver_ = nullptr;
    DocumentRecomputeEngine engine_{*this};
};

} // namespace paramcad
