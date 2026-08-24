#include "Core/Document/DocumentBase.h"

#include "Core/Geometry/Transform.h"
#include "Core/Recompute/IRecomputable.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace paramcad {

DocumentBase::DocumentBase(std::string name) : CadDocument(std::move(name)) {}

DocumentBase::DocumentBase(ObjectId id, std::string name) : CadDocument(id, std::move(name)) {}

bool DocumentBase::restoreRemoveObject(ObjectId id) {
    const bool wasApplying = applyingHistory_;
    applyingHistory_ = true;
    const bool ok = removeObject(id);
    applyingHistory_ = wasApplying;
    return ok;
}

DocumentRecomputeReport DocumentBase::recompute() { return engine_.recompute(); }

DocumentRecomputeReport DocumentBase::recomputeFrom(ObjectId id) {
    return engine_.recomputeFrom(id);
}

void DocumentBase::createOriginFrame() {
    // The Origin frame is part of CONSTRUCTING a document, not something the
    // user did, so it records nothing. Without this every document -- including
    // one that has just been loaded -- arrived carrying one undo step, and
    // "Undo" on a freshly opened part would delete its Origin.
    //
    // Caught by M9_UNDO_402 and GATE_G2 the moment frames became recordable,
    // which is the second time the "a loaded document starts empty" rule has
    // paid for itself in as many milestones.
    const bool wasApplying = applyingHistory_;
    applyingHistory_ = true;
    addFrame("Origin");
    applyingHistory_ = wasApplying;
}

std::vector<const ReferenceFrame*> DocumentBase::frames() const {
    std::vector<const ReferenceFrame*> result;
    result.reserve(frames_.size());
    for (const std::unique_ptr<ReferenceFrame>& frame : frames_) result.push_back(frame.get());
    return result;
}

const ReferenceFrame* DocumentBase::findFrame(ObjectId id) const noexcept {
    for (const std::unique_ptr<ReferenceFrame>& frame : frames_)
        if (frame->id() == id) return frame.get();
    return nullptr;
}

// Would making `frameId`'s parent `parentFrameId` close a loop? Walks UP from
// the proposed parent looking for the child. Bounded by the frame count: a
// cycle that already existed would otherwise spin here for ever, and refusing
// to create one is worth nothing if the check itself can hang (M9.1's lesson,
// paid once).
void DocumentBase::requireAcyclicParent(ObjectId frameId, ObjectId parentFrameId) const {
    if (parentFrameId == kInvalidObjectId) return;
    if (findFrame(parentFrameId) == nullptr)
        throw std::runtime_error("frame parent " + std::to_string(parentFrameId) +
                                 " is not a reference frame in this document");
    ObjectId walk = parentFrameId;
    for (std::size_t step = 0; step <= frames_.size(); ++step) {
        if (walk == kInvalidObjectId) return;
        if (walk == frameId)
            throw std::runtime_error("frame " + std::to_string(frameId) +
                                     " cannot be parented to " +
                                     std::to_string(parentFrameId) +
                                     ": the parent chain would cycle");
        const ReferenceFrame* parent = findFrame(walk);
        if (parent == nullptr) return;
        walk = parent->parentFrameId();
    }
    throw std::runtime_error("frame parent chain is already cyclic");
}

ReferenceFrame& DocumentBase::addFrame(std::string name, ObjectId parentFrameId) {
    requireAcyclicParent(kInvalidObjectId, parentFrameId);
    auto item = std::make_unique<ReferenceFrame>(std::move(name), parentFrameId);
    auto& ref = *item;
    frames_.push_back(std::move(item));
    // FIRST-CLASS since M10 (ADR-M10-001): resolvable by id, and a graph node
    // so that moving it dirties whatever it supports. Before M10 this function
    // stopped at the push_back, which is why a frame had an ObjectId nothing
    // could resolve and `removeObject` could not see it.
    registry_.registerObject(ref.id(), &ref);
    graph_.addNode(ref.id());
    if (parentFrameId != kInvalidObjectId) graph_.addDependency(ref.id(), parentFrameId);
    FrameExistenceEdit edit;
    edit.frameId = ref.id();
    edit.name = ref.name();
    edit.parentFrameId = parentFrameId;
    edit.localTransform = ref.localTransform();
    edit.addedByTheEdit = true;
    recordDelta(edit, "Add " + ref.name());
    return ref;
}

ReferenceFrame& DocumentBase::restoreFrame(ObjectId id, std::string name, ObjectId parentFrameId,
                                           const Transform3D& localTransform) {
    requireUnusedId(id, "restoreFrame");
    requireAcyclicParent(id, parentFrameId);
    auto item = std::make_unique<ReferenceFrame>(id, std::move(name), parentFrameId,
                                                 localTransform);
    auto& ref = *item;
    frames_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    graph_.addNode(ref.id());
    if (parentFrameId != kInvalidObjectId) graph_.addDependency(ref.id(), parentFrameId);
    return ref; // NOT recorded: deserialization is not a user edit (ADR-M9-001)
}

bool DocumentBase::setFrameTransform(ObjectId frameId, const Transform3D& transform) {
    ReferenceFrame* frame = nullptr;
    for (const std::unique_ptr<ReferenceFrame>& candidate : frames_)
        if (candidate->id() == frameId) frame = candidate.get();
    if (frame == nullptr) return false;

    FrameTransformEdit edit;
    edit.frameId = frameId;
    edit.before = frame->localTransform();
    edit.after = transform;
    frame->setLocalTransform(transform);
    // Dirty the FRAME, and let the graph carry it to the sketches this frame
    // supports and the features built on those. Nothing walks the tree here:
    // the parent edge and the frame -> sketch edge are ordinary graph edges, so
    // a grandparent move reaches a grandchild's pad by the same machinery a
    // Parameter edit uses (ADR-M10-002).
    graph_.markDirty(frameId);
    onGraphDirtied();
    recordDelta(edit, "Move " + frame->name());
    return true;
}

bool DocumentBase::setFrameParent(ObjectId frameId, ObjectId parentFrameId) {
    ReferenceFrame* frame = nullptr;
    for (const std::unique_ptr<ReferenceFrame>& candidate : frames_)
        if (candidate->id() == frameId) frame = candidate.get();
    if (frame == nullptr) return false;
    requireAcyclicParent(frameId, parentFrameId);

    const ObjectId before = frame->parentFrameId();
    if (before == parentFrameId) return true;
    if (before != kInvalidObjectId) graph_.removeDependency(frameId, before);
    frame->setParentFrameId(parentFrameId);
    if (parentFrameId != kInvalidObjectId) graph_.addDependency(frameId, parentFrameId);
    graph_.markDirty(frameId);
    onGraphDirtied();

    FrameParentEdit edit;
    edit.frameId = frameId;
    edit.before = before;
    edit.after = parentFrameId;
    recordDelta(edit, "Reparent " + frame->name());
    return true;
}

bool DocumentBase::restoreFrameParent(ObjectId frameId, ObjectId parentFrameId) {
    const bool wasApplying = applyingHistory_;
    applyingHistory_ = true;
    const bool ok = setFrameParent(frameId, parentFrameId);
    applyingHistory_ = wasApplying;
    return ok;
}

Transform3D DocumentBase::worldTransform(ObjectId frameId) const noexcept {
    // COMPOSED, never stored (ADR-M10-002). Walked from the frame up to the
    // root and composed on the way back down, so a parent's rotation applies to
    // a child's offset rather than merely being added to it.
    std::vector<const ReferenceFrame*> chain;
    ObjectId walk = frameId;
    for (std::size_t step = 0; step <= frames_.size(); ++step) {
        const ReferenceFrame* frame = findFrame(walk);
        if (frame == nullptr) break;
        chain.push_back(frame);
        walk = frame->parentFrameId();
        if (walk == kInvalidObjectId) break;
    }
    Transform3D world = Transform3D::Identity();
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        world = Compose(world, (*it)->localTransform());
    return world;
}

Connector& DocumentBase::addConnector(std::string name, ConnectorRole role, ObjectId frameId,
                                      ConnectorOwner owner) {
    if (findFrame(frameId) == nullptr)
        throw std::runtime_error("addConnector: frame " + std::to_string(frameId) +
                                 " is not a reference frame in this document");
    requireUnusedConnectorName(name, "addConnector");
    auto item = std::make_unique<Connector>(std::move(name), role, frameId, owner);
    auto& ref = *item;
    connectors_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);

    ConnectorExistenceEdit edit;
    edit.connectorId = ref.id();
    edit.name = ref.name();
    edit.role = static_cast<int>(role);
    edit.frameId = frameId;
    edit.owner = static_cast<int>(owner);
    edit.addedByTheEdit = true;
    recordDelta(edit, "Add " + ref.name());
    return ref;
}

Connector& DocumentBase::restoreConnector(ObjectId id, std::string name, ConnectorRole role,
                                          ObjectId frameId, ConnectorOwner owner) {
    requireUnusedId(id, "restoreConnector");
    if (findFrame(frameId) == nullptr)
        throw std::runtime_error("restoreConnector: frame " + std::to_string(frameId) +
                                 " is not a reference frame in this document");
    requireUnusedConnectorName(name, "restoreConnector");
    auto item = std::make_unique<Connector>(id, std::move(name), role, frameId, owner);
    auto& ref = *item;
    connectors_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    return ref; // NOT recorded: deserialization is not a user edit (ADR-M9-001)
}

// A CONNECTOR NAME IS A REFERENCE, so it has to be unique (M25).
//
// A mate names its two ends by connector NAME across a document boundary
// (ADR-M24-001): there is no id to point at, because the connector lives in the
// part file and every instance of that part reuses it. Two connectors sharing a
// name therefore make a mate mean whichever one comes first -- which is
// position as identity, the thing ADR-M4-004 exists to forbid, arriving by a
// route nobody had checked.
//
// Found by running examples/four-bar.ep3ds: one script drew four links into one
// document, called every link's ends "A" and "B", and every mate in the linkage
// quietly resolved to the first link's. The parts were placed at angles nobody
// had asked for and the solve reported success.
//
// The general rename rule (`renameObject`) already refused a duplicate. This is
// the same rule at the OTHER door, which is where it was missing.
void DocumentBase::requireUnusedConnectorName(const std::string& name, const char* who) const {
    for (const std::unique_ptr<Connector>& existing : connectors_)
        if (existing->name() == name)
            throw std::runtime_error(std::string(who) + ": there is already a connector called '" +
                                     name + "' in this document, and a mate names one by name");
}

std::vector<const Connector*> DocumentBase::connectors() const {
    std::vector<const Connector*> result;
    result.reserve(connectors_.size());
    for (const std::unique_ptr<Connector>& c : connectors_) result.push_back(c.get());
    return result;
}

const Connector* DocumentBase::findConnector(ObjectId id) const noexcept {
    for (const std::unique_ptr<Connector>& c : connectors_)
        if (c->id() == id) return c.get();
    return nullptr;
}

Transform3D DocumentBase::connectorWorldTransform(ObjectId connectorId) const noexcept {
    // A connector holds no geometry of its own (ADR-M10-004): it IS its frame,
    // plus meaning. So this is the frame's world transform, and a connector on
    // a moved frame reports the moved place with nothing to keep in step.
    const Connector* connector = findConnector(connectorId);
    if (connector == nullptr) return Transform3D::Identity();
    return worldTransform(connector->frameId());
}

void DocumentBase::recordDelta(UndoDelta delta, std::string label) {
    // Undo and redo drive these very facade methods. Without this guard the
    // first undo would record its own inverse and the stack would oscillate
    // forever between two states.
    if (applyingHistory_) return;
    if (openTransaction_.has_value()) {
        openTransaction_->deltas.push_back(std::move(delta));
        return;
    }
    UndoRecord record;
    record.label = std::move(label);
    record.deltas.push_back(std::move(delta));
    undoStack_.push_back(std::move(record));
    // A new edit after an undo DISCARDS the redo branch. Keeping it would let
    // a redo replay a change against a document that has since moved, which is
    // the one way an undo system can silently produce a state the user never
    // had.
    redoStack_.clear();
}

void DocumentBase::clearHistory(const char* becauseOfWhat) noexcept {
    // Named argument, unused at runtime: it exists so every call site has to
    // say WHY it is throwing the history away, in the code rather than in a
    // comment somewhere else.
    (void)becauseOfWhat;
    undoStack_.clear();
    redoStack_.clear();
    openTransaction_.reset();
}

void DocumentBase::beginTransaction(std::string label) {
    // A second begin closes nothing and loses nothing: the outer transaction
    // simply keeps collecting. Nesting is not a feature M9.1 offers, and
    // silently discarding the outer record would be the dangerous reading.
    if (openTransaction_.has_value()) return;
    UndoRecord record;
    record.label = std::move(label);
    openTransaction_ = std::move(record);
}

bool DocumentBase::commitTransaction() {
    if (!openTransaction_.has_value()) return false;
    UndoRecord record = std::move(*openTransaction_);
    openTransaction_.reset();
    // An empty transaction is not an undo step. "The user opened a dialog and
    // pressed OK without changing anything" must not consume one.
    if (record.deltas.empty()) return true;
    undoStack_.push_back(std::move(record));
    redoStack_.clear();
    return true;
}

bool DocumentBase::abortTransaction() {
    if (!openTransaction_.has_value()) return false;
    const UndoRecord record = std::move(*openTransaction_);
    openTransaction_.reset();
    // Nothing is recorded: the document is left as if the transaction had
    // never started (M9 spec section 6). The deltas are undone in REVERSE, for
    // the same reason undo() does it.
    applyingHistory_ = true;
    for (auto it = record.deltas.rbegin(); it != record.deltas.rend(); ++it)
        applyDelta(*it, /*forward=*/false);
    applyingHistory_ = false;
    return true;
}

bool DocumentBase::undo() {
    // An open transaction is not undoable -- it is not a step yet. Abort it
    // first, or commit it.
    if (openTransaction_.has_value()) return false;
    if (undoStack_.empty()) return false; // a no-op, never a corruption
    UndoRecord record = std::move(undoStack_.back());
    undoStack_.pop_back();
    applyingHistory_ = true;
    for (auto it = record.deltas.rbegin(); it != record.deltas.rend(); ++it)
        applyDelta(*it, /*forward=*/false);
    applyingHistory_ = false;
    redoStack_.push_back(std::move(record));
    return true;
}

bool DocumentBase::redo() {
    if (openTransaction_.has_value()) return false;
    if (redoStack_.empty()) return false;
    UndoRecord record = std::move(redoStack_.back());
    redoStack_.pop_back();
    applyingHistory_ = true;
    for (const UndoDelta& delta : record.deltas) applyDelta(delta, /*forward=*/true);
    applyingHistory_ = false;
    undoStack_.push_back(std::move(record));
    return true;
}

std::string DocumentBase::nextUndoLabel() const {
    return undoStack_.empty() ? std::string() : undoStack_.back().label;
}

std::string DocumentBase::nextRedoLabel() const {
    return redoStack_.empty() ? std::string() : redoStack_.back().label;
}

void DocumentBase::requireUnusedId(ObjectId id, const char* who) const {
    // Half zero: the document's OWN id. A document does not register
    // itself, so it is the registry's third blind spot (round 4, R2R4-m1) --
    // restoring anything onto it built cleanly and then refused to save, for
    // ever. Cheapest of the three checks, so it goes first.
    if (id == this->id())
        throw std::runtime_error(std::string(who) + ": id " + std::to_string(id) +
                                 " is already used by this document itself");
    // Half one: every REGISTERED object -- parameters, materials, bodies,
    // sketches, and every feature that reached the registry.
    if (registry_.contains(id))
        throw std::runtime_error(std::string(who) + ": id " + std::to_string(id) +
                                 " is already registered in this document");
    // Half two: ids the registry CANNOT see, which only the subclass knows
    // about -- a Part's PlaceholderFeature is deliberately never registered
    // (ADR-009 D4), so without this a placeholder-held id passes half one and
    // collides anyway. That is exactly how round 4's R1R4-C1 built two
    // features with one ObjectId in one Body through public calls alone.
    requireUnusedIdHook(id, who);
}

GraphResult DocumentBase::addRecomputableNode(IRecomputable& object) {
    if (!registry_.registerObject(object.id(), &object)) {
        return {object.id() == kInvalidObjectId ? GraphError::NodeNotFound
                                                : GraphError::NodeAlreadyExists};
    }
    const GraphResult result = graph_.addNode(object.id());
    if (!result) registry_.unregisterObject(object.id()); // keep the two in sync
    return result;
}

GraphResult DocumentBase::addDependency(ObjectId dependent, ObjectId prerequisite) {
    return graph_.addDependency(dependent, prerequisite);
}

GraphResult DocumentBase::removeDependency(ObjectId dependent, ObjectId prerequisite) {
    return graph_.removeDependency(dependent, prerequisite);
}

bool DocumentBase::markDirty(ObjectId id) {
    if (!graph_.markDirty(id)) return false;
    onGraphDirtied();
    return true;
}

// --- Naming: one path, whatever kind of document this is (M23) --------------

std::string DocumentBase::objectName(ObjectId id) const {
    // The base's OWN objects first. Frames and connectors were unnameable
    // before M23 -- `renameObject` refused them, which nobody had noticed
    // because no UI offered it -- and an assembly instance IS a frame plus a
    // source, so the gap had to close here or an instance could not be named.
    for (const std::unique_ptr<ReferenceFrame>& frame : frames_)
        if (frame->id() == id) return frame->name();
    for (const std::unique_ptr<Connector>& connector : connectors_)
        if (connector->id() == id) return connector->name();
    return ownObjectName(id);
}

void DocumentBase::applyName(ObjectId id, const std::string& name) {
    for (const std::unique_ptr<ReferenceFrame>& frame : frames_)
        if (frame->id() == id) {
            frame->setName(name);
            return;
        }
    for (const std::unique_ptr<Connector>& connector : connectors_)
        if (connector->id() == id) {
            connector->setName(name);
            return;
        }
    applyOwnName(id, name);
}

bool DocumentBase::nameIsTaken(const std::string& name, ObjectId except) const {
    for (const std::unique_ptr<ReferenceFrame>& frame : frames_)
        if (frame->id() != except && frame->name() == name) return true;
    for (const std::unique_ptr<Connector>& connector : connectors_)
        if (connector->id() != except && connector->name() == name) return true;
    return ownNameIsTaken(name, except);
}

DocumentBase::RenameResult DocumentBase::renameObject(ObjectId id, std::string name) {
    // Trimmed, because a name that differs from another only by a space it is
    // impossible to see is not a name a user can tell apart.
    const std::size_t first = name.find_first_not_of(" \t");
    const std::size_t last = name.find_last_not_of(" \t");
    name = first == std::string::npos ? std::string() : name.substr(first, last - first + 1);
    if (name.empty()) return RenameResult{false, "a name cannot be empty"};

    const std::string current = objectName(id);
    if (current.empty()) return RenameResult{false, "that object cannot be renamed"};
    if (current == name) return RenameResult{true, {}}; // nothing to record

    if (nameIsTaken(name, id)) return RenameResult{false, "'" + name + "' is already taken"};

    ObjectNameEdit edit;
    edit.objectId = id;
    edit.before = current;
    edit.after = name;
    applyName(id, name);
    recordDelta(edit, "Rename to " + name);
    return RenameResult{true, {}};
}

// --- Undo: the base's own deltas, then the subclass's (M23) -----------------

bool DocumentBase::applyBaseDelta(const UndoDelta& delta, bool forward) {
    if (const auto* edit = std::get_if<ObjectNameEdit>(&delta)) {
        // Through the same private writer the facade uses, so undo cannot set
        // a name the facade would have refused -- and NOT dirtying anything: a
        // name has no geometric consequence, and marking the object dirty
        // would rebuild the whole chain below it to produce identical shapes.
        applyName(edit->objectId, forward ? edit->after : edit->before);
        return true;
    }
    if (const auto* edit = std::get_if<ConnectorExistenceEdit>(&delta)) {
        const bool shouldExist = forward ? edit->addedByTheEdit : !edit->addedByTheEdit;
        const bool doesExist = findConnector(edit->connectorId) != nullptr;
        if (shouldExist == doesExist) return true;
        if (shouldExist)
            restoreConnector(edit->connectorId, edit->name,
                             static_cast<ConnectorRole>(edit->role), edit->frameId,
                             static_cast<ConnectorOwner>(edit->owner));
        else
            removeObject(edit->connectorId);
        return true;
    }
    if (const auto* edit = std::get_if<FrameTransformEdit>(&delta)) {
        setFrameTransform(edit->frameId, forward ? edit->after : edit->before);
        return true;
    }
    if (const auto* edit = std::get_if<FrameParentEdit>(&delta)) {
        setFrameParent(edit->frameId, forward ? edit->after : edit->before);
        return true;
    }
    if (const auto* edit = std::get_if<FrameExistenceEdit>(&delta)) {
        const bool shouldExist = forward ? edit->addedByTheEdit : !edit->addedByTheEdit;
        const bool doesExist = findFrame(edit->frameId) != nullptr;
        if (shouldExist == doesExist) return true;
        if (shouldExist)
            restoreFrame(edit->frameId, edit->name, edit->parentFrameId, edit->localTransform);
        else
            removeObject(edit->frameId);
        return true;
    }
    return false;
}

void DocumentBase::applyDelta(const UndoDelta& delta, bool forward) {
    if (applyBaseDelta(delta, forward)) return;
    applyOwnDelta(delta, forward);
}

// --- Removing a frame or a connector, in one place --------------------------

void DocumentBase::recordBaseRemoval(const ObjectRegistry::ObjectRef& handle, ObjectId id) {
    // A frame's removal is recordable, so deleting one does not end the
    // history (M10). What it does NOT do is cascade: whatever referenced the
    // frame FAILS loudly and save refuses the dangling reference, which is the
    // accepted M4 precedent for deleting a sketch a Pad reads.
    if (applyingHistory_) return;
    if (std::holds_alternative<Connector*>(handle)) {
        const Connector* connector = std::get<Connector*>(handle);
        ConnectorExistenceEdit edit;
        edit.connectorId = id;
        edit.name = connector->name();
        edit.role = static_cast<int>(connector->role());
        edit.frameId = connector->frameId();
        edit.owner = static_cast<int>(connector->owner());
        edit.addedByTheEdit = false;
        recordDelta(edit, "Delete " + connector->name());
        return;
    }
    if (std::holds_alternative<ReferenceFrame*>(handle)) {
        const ReferenceFrame* frame = std::get<ReferenceFrame*>(handle);
        FrameExistenceEdit edit;
        edit.frameId = id;
        edit.name = frame->name();
        edit.parentFrameId = frame->parentFrameId();
        edit.localTransform = frame->localTransform();
        edit.addedByTheEdit = false;
        recordDelta(edit, "Delete " + frame->name());
    }
}

bool DocumentBase::eraseBaseOwned(const ObjectRegistry::ObjectRef& handle, ObjectId id) {
    if (std::holds_alternative<ReferenceFrame*>(handle)) {
        for (auto it = frames_.begin(); it != frames_.end(); ++it)
            if ((*it)->id() == id) {
                frames_.erase(it);
                return true;
            }
        return true;
    }
    if (std::holds_alternative<Connector*>(handle)) {
        for (auto it = connectors_.begin(); it != connectors_.end(); ++it)
            if ((*it)->id() == id) {
                connectors_.erase(it);
                return true;
            }
        return true;
    }
    return false;
}

} // namespace paramcad
