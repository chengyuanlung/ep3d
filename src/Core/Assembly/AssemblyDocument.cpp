#include "Core/Assembly/AssemblyDocument.h"

#include "Core/Geometry/Transform.h"

#include <stdexcept>
#include <utility>

namespace paramcad {

namespace {

// The frame that places an instance is named after it, so a tree and an undo
// label read as sentences rather than as ids. Derived in one place because two
// spellings of the same rule is how a rename leaves a frame behind carrying
// the old name.
std::string FrameNameFor(const std::string& instanceName) { return instanceName + " origin"; }

} // namespace

AssemblyDocument::AssemblyDocument(std::string name) : DocumentBase(std::move(name)) {
    createOriginFrame();
}

AssemblyDocument::AssemblyDocument(ObjectId id, std::string name)
    : DocumentBase(id, std::move(name)) {
    createOriginFrame();
}

void AssemblyDocument::wireInstance(PartInstance& instance) {
    addRecomputableNode(instance);
    // THE ONE EDGE AN INSTANCE HAS: its frame. Moving the frame dirties the
    // instance through the ordinary graph, so nothing here walks anything and
    // a sub-assembly in M26 -- a frame parented to a frame -- reaches its
    // children by the same machinery a Parameter edit uses (ADR-M10-002).
    //
    // There is deliberately NO edge for the source file, for the same reason
    // an ImportFeature has none (ADR-M22-003): the graph tracks objects in
    // this document, and a file is not one of them. Editing the part does not
    // dirty the instance; the next rebuild picks it up.
    if (instance.frameId() != kInvalidObjectId)
        addDependency(instance.id(), instance.frameId());
}

PartInstance& AssemblyDocument::addInstance(std::string name, std::string sourcePath,
                                           std::string bodyName) {
    if (sourcePath.empty())
        throw std::runtime_error("addInstance: an instance must name a part file");
    // The frame FIRST, so the instance never exists without a place to be.
    ReferenceFrame& placement = addFrame(FrameNameFor(name), kInvalidObjectId);
    auto item = std::make_unique<PartInstance>(std::move(name), std::move(sourcePath),
                                              std::move(bodyName), placement.id());
    auto& ref = *item;
    instances_.push_back(std::move(item));
    wireInstance(ref);

    InstanceExistenceEdit edit;
    edit.instanceId = ref.id();
    edit.name = ref.name();
    edit.sourcePath = ref.sourcePath();
    edit.bodyName = ref.bodyName();
    edit.frameId = ref.frameId();
    edit.addedByTheEdit = true;
    recordDelta(edit, "Insert " + ref.name());
    return ref;
}

PartInstance& AssemblyDocument::restoreInstance(ObjectId id, std::string name, ComputeState state,
                                               std::string sourcePath, std::string bodyName,
                                               ObjectId frameId) {
    requireUnusedId(id, "restoreInstance");
    if (findFrame(frameId) == nullptr)
        throw std::runtime_error("restoreInstance: frame " + std::to_string(frameId) +
                                 " is not a reference frame in this document");
    auto item = std::make_unique<PartInstance>(id, std::move(name), state, std::move(sourcePath),
                                              std::move(bodyName), frameId);
    auto& ref = *item;
    instances_.push_back(std::move(item));
    wireInstance(ref);
    return ref; // NOT recorded: deserialization is not a user edit (ADR-M9-001)
}

std::vector<const PartInstance*> AssemblyDocument::instances() const {
    std::vector<const PartInstance*> result;
    result.reserve(instances_.size());
    for (const std::unique_ptr<PartInstance>& one : instances_) result.push_back(one.get());
    return result;
}

const PartInstance* AssemblyDocument::findInstance(ObjectId id) const noexcept {
    for (const std::unique_ptr<PartInstance>& one : instances_)
        if (one->id() == id) return one.get();
    return nullptr;
}

const PartInstance* AssemblyDocument::findInstanceNamed(const std::string& name) const noexcept {
    for (const std::unique_ptr<PartInstance>& one : instances_)
        if (one->name() == name) return one.get();
    return nullptr;
}

PartInstance* AssemblyDocument::findInstanceForEdit(ObjectId id) noexcept {
    for (const std::unique_ptr<PartInstance>& one : instances_)
        if (one->id() == id) return one.get();
    return nullptr;
}

bool AssemblyDocument::setInstanceTransform(ObjectId instanceId, const Transform3D& placement) {
    const PartInstance* instance = findInstance(instanceId);
    if (instance == nullptr) return false;
    // Through the FRAME. Not "as well as" -- there is nowhere else a placement
    // is kept, so this cannot leave two answers behind.
    return setFrameTransform(instance->frameId(), placement);
}

Transform3D AssemblyDocument::instanceTransform(ObjectId instanceId) const noexcept {
    const PartInstance* instance = findInstance(instanceId);
    if (instance == nullptr) return Transform3D::Identity();
    const ReferenceFrame* frame = findFrame(instance->frameId());
    return frame == nullptr ? Transform3D::Identity() : frame->localTransform();
}

Transform3D AssemblyDocument::instanceWorldTransform(ObjectId instanceId) const noexcept {
    const PartInstance* instance = findInstance(instanceId);
    if (instance == nullptr) return Transform3D::Identity();
    return worldTransform(instance->frameId());
}

// --- What this document owes DocumentBase ------------------------------------

void AssemblyDocument::requireUnusedIdHook(ObjectId id, const char* who) const {
    // An assembly has no unregistered objects -- every instance reaches the
    // registry through addRecomputableNode -- so there is nothing the base's
    // registry check cannot already see. Stated rather than left blank,
    // because a silently empty hook and a deliberately empty one look the same
    // and only one of them is right.
    (void)id;
    (void)who;
}

std::string AssemblyDocument::ownObjectName(ObjectId id) const {
    const PartInstance* instance = findInstance(id);
    return instance == nullptr ? std::string() : instance->name();
}

void AssemblyDocument::applyOwnName(ObjectId id, const std::string& name) {
    PartInstance* instance = findInstanceForEdit(id);
    if (instance == nullptr) return;
    instance->setName(name);
    // The placement frame is named after the instance, so it follows. Written
    // here rather than left alone because a tree that showed "Gear origin"
    // under an instance called "Pinion" would be describing a document that
    // does not exist. Through the base's own writer, so there is still only
    // one thing that can set a frame's name.
    applyName(instance->frameId(), FrameNameFor(name));
}

bool AssemblyDocument::ownNameIsTaken(const std::string& name, ObjectId except) const {
    for (const std::unique_ptr<PartInstance>& one : instances_)
        if (one->id() != except && one->name() == name) return true;
    return false;
}

void AssemblyDocument::applyOwnDelta(const UndoDelta& delta, bool forward) {
    if (const auto* edit = std::get_if<InstanceExistenceEdit>(&delta)) {
        const bool shouldExist = forward ? edit->addedByTheEdit : !edit->addedByTheEdit;
        const bool doesExist = findInstance(edit->instanceId) != nullptr;
        if (shouldExist == doesExist) return;
        if (shouldExist)
            restoreInstance(edit->instanceId, edit->name, ComputeState::Dirty, edit->sourcePath,
                            edit->bodyName, edit->frameId);
        else
            removeObject(edit->instanceId);
        return;
    }
    // A delta this document cannot replay. THROWS rather than returning,
    // because a silently skipped delta is an undo that half-happened -- the
    // user sees some of their change come back and none of the rest, with
    // nothing said. The Part side has the same property, by way of the
    // std::get at the end of its chain.
    throw std::runtime_error("this assembly cannot undo a change of that kind");
}

bool AssemblyDocument::removeObject(ObjectId id) {
    ObjectRegistry::ObjectRef* found = registry_.find(id);
    if (found == nullptr) return false;
    const ObjectRegistry::ObjectRef handle = *found; // copy before unregistering

    // Decided BEFORE anything is unhooked, so a refusal leaves the document
    // byte-for-byte unchanged.
    const PartInstance* instance = findInstance(id);
    if (instance != nullptr && !applyingHistory()) {
        InstanceExistenceEdit edit;
        edit.instanceId = id;
        edit.name = instance->name();
        edit.sourcePath = instance->sourcePath();
        edit.bodyName = instance->bodyName();
        edit.frameId = instance->frameId();
        edit.addedByTheEdit = false;
        recordDelta(edit, "Delete " + instance->name());
    }
    recordBaseRemoval(handle, id);

    // Order matters (spec 12): graph first (edges cleaned in both directions,
    // former dependents dirtied per ADR-007), then registry, then owner.
    graph_.removeNode(id);
    registry_.unregisterObject(id);

    if (instance != nullptr) {
        const ObjectId placementFrame = instance->frameId();
        for (auto it = instances_.begin(); it != instances_.end(); ++it)
            if ((*it)->id() == id) {
                instances_.erase(it);
                break;
            }
        // THE FRAME GOES WITH IT. An instance's frame exists only to place
        // that instance -- nothing else can reference it -- so leaving it
        // behind would put an orphan in the tree that a user cannot explain
        // and that the next save would faithfully preserve for ever.
        //
        // This is a cascade, and cascades are refused elsewhere in this
        // project on purpose (a sketch a Pad reads is NOT deleted with it).
        // The difference is ownership: that sketch is a shared, named,
        // separately created object, and this frame is a private one this
        // document made and nobody else can name.
        if (placementFrame != kInvalidObjectId && findFrame(placementFrame) != nullptr)
            removeObject(placementFrame);
        return true;
    }

    return eraseBaseOwned(handle, id) || true;
}

} // namespace paramcad
