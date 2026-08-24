#include "Core/Assembly/AssemblyDocument.h"

#include "Core/Geometry/Transform.h"

#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace paramcad {

namespace {

// The frame that places an instance is named after it, so a tree and an undo
// label read as sentences rather than as ids. Derived in one place because two
// spellings of the same rule is how a rename leaves a frame behind carrying
// the old name.
std::string FrameNameFor(const std::string& instanceName) { return instanceName + " origin"; }

// Are these the same placement, to within what a double can carry through a
// compose and an inverse? Used ONLY to decide whether the solve needs to move
// anything -- never to decide whether a mate is satisfied, because a solve
// that asked "close enough?" about its own answer would be measuring its own
// rounding.
bool SameTransform(const Transform3D& a, const Transform3D& b) noexcept {
    const auto near = [](double x, double y) { return std::fabs(x - y) <= 1e-12; };
    return near(a.translation.x, b.translation.x) && near(a.translation.y, b.translation.y) &&
           near(a.translation.z, b.translation.z) && near(a.rotation.w, b.rotation.w) &&
           near(a.rotation.x, b.rotation.x) && near(a.rotation.y, b.rotation.y) &&
           near(a.rotation.z, b.rotation.z);
}

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

Transform3D AssemblyDocument::mateConnectorWorldTransform(ObjectId instanceId,
                                                          const std::string& connectorName,
                                                          bool* found) const noexcept {
    if (found != nullptr) *found = false;
    const PartInstance* instance = findInstance(instanceId);
    if (instance == nullptr) return Transform3D::Identity();
    const PartInstance::MateConnector* connector = instance->findConnector(connectorName);
    if (connector == nullptr) return Transform3D::Identity();
    if (found != nullptr) *found = true;
    return Compose(instanceWorldTransform(instanceId), connector->localTransform);
}

// --- Grounding (M24) ---------------------------------------------------------

bool AssemblyDocument::isInstanceGrounded(ObjectId instanceId) const noexcept {
    for (const ObjectId one : groundedInstances_)
        if (one == instanceId) return true;
    return false;
}

bool AssemblyDocument::setInstanceGrounded(ObjectId instanceId, bool grounded) {
    if (findInstance(instanceId) == nullptr) return false;
    const bool was = isInstanceGrounded(instanceId);
    if (was == grounded) return true;

    if (grounded) {
        groundedInstances_.push_back(instanceId);
    } else {
        for (auto it = groundedInstances_.begin(); it != groundedInstances_.end(); ++it)
            if (*it == instanceId) {
                groundedInstances_.erase(it);
                break;
            }
    }
    // Dirty the instance so the next pass re-solves from the new ground.
    // Through the graph, like everything else here.
    graph_.markDirty(instanceId);

    InstanceGroundEdit edit;
    edit.instanceId = instanceId;
    edit.before = was;
    edit.after = grounded;
    recordDelta(edit, (grounded ? "Ground " : "Release ") + objectName(instanceId));
    return true;
}

bool AssemblyDocument::restoreInstanceGrounded(ObjectId instanceId) {
    const bool wasApplying = applyingHistory_;
    applyingHistory_ = true;
    const bool ok = setInstanceGrounded(instanceId, true);
    applyingHistory_ = wasApplying;
    return ok;
}

// --- Mates -------------------------------------------------------------------

// NOT A GRAPH NODE, and that is a decision worth stating.
//
// A mate does not BUILD anything: it decides where instances go, and the
// instances are the nodes that build. Giving a mate a node would mean the
// graph had to run it before the instances it places, which is true, and
// then the instances would have to depend on it, which is also true --
// and then a mate that reads an instance's connectors would depend on the
// instance right back. A cycle, in a graph whose whole job is to refuse
// those.
//
// So the mate solve is a pass over the mate graph, run by recompute()
// between two graph passes, and the graph never sees a mate at all. What
// the graph DOES see is the frames the solve writes, which dirty exactly
// the instances that moved.
Mate& AssemblyDocument::addMate(std::string name, MateType type, ObjectId leadingInstanceId,
                                std::string leadingConnector, ObjectId followingInstanceId,
                                std::string followingConnector, double value) {
    requireMatable(leadingInstanceId, followingInstanceId, type, value, "addMate");
    auto item = std::make_unique<Mate>(std::move(name), type, leadingInstanceId,
                                       std::move(leadingConnector), followingInstanceId,
                                       std::move(followingConnector), value);
    auto& ref = *item;
    mates_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    graph_.markDirty(followingInstanceId);

    MateExistenceEdit edit;
    edit.mateId = ref.id();
    edit.name = ref.name();
    edit.type = static_cast<int>(ref.type());
    edit.leadingInstanceId = ref.leadingInstanceId();
    edit.leadingConnector = ref.leadingConnector();
    edit.followingInstanceId = ref.followingInstanceId();
    edit.followingConnector = ref.followingConnector();
    edit.value = ref.value();
    edit.addedByTheEdit = true;
    recordDelta(edit, "Add " + ref.name());
    return ref;
}

Mate& AssemblyDocument::restoreMate(ObjectId id, std::string name, MateType type,
                                    ObjectId leadingInstanceId, std::string leadingConnector,
                                    ObjectId followingInstanceId, std::string followingConnector,
                                    double value) {
    requireUnusedId(id, "restoreMate");
    requireMatable(leadingInstanceId, followingInstanceId, type, value, "restoreMate");
    auto item = std::make_unique<Mate>(id, std::move(name), type, leadingInstanceId,
                                       std::move(leadingConnector), followingInstanceId,
                                       std::move(followingConnector), value);
    auto& ref = *item;
    mates_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    return ref; // NOT recorded: deserialization is not a user edit (ADR-M9-001)
}

void AssemblyDocument::requireMatable(ObjectId leadingInstanceId, ObjectId followingInstanceId,
                                      MateType type, double value, const char* who) const {
    if (findInstance(leadingInstanceId) == nullptr)
        throw std::runtime_error(std::string(who) + ": " + std::to_string(leadingInstanceId) +
                                 " is not an instance in this assembly");
    if (findInstance(followingInstanceId) == nullptr)
        throw std::runtime_error(std::string(who) + ": " + std::to_string(followingInstanceId) +
                                 " is not an instance in this assembly");
    // A THING CANNOT BE MATED TO ITSELF. The solve would place it from its own
    // placement, which is either a no-op or a contradiction depending on the
    // value, and neither is what anybody meant.
    if (leadingInstanceId == followingInstanceId)
        throw std::runtime_error(std::string(who) +
                                 ": an instance cannot be mated to itself");
    // A VALUE ON A FASTENED MATE is refused rather than ignored: ignoring it
    // would leave the writer believing they had offset something.
    if (type == MateType::Fastened && value != 0.0)
        throw std::runtime_error(std::string(who) +
                                 ": a fastened mate has no freedom to give a value to");
}

std::vector<const Mate*> AssemblyDocument::mates() const {
    std::vector<const Mate*> result;
    result.reserve(mates_.size());
    for (const std::unique_ptr<Mate>& one : mates_) result.push_back(one.get());
    return result;
}

const Mate* AssemblyDocument::findMate(ObjectId id) const noexcept {
    for (const std::unique_ptr<Mate>& one : mates_)
        if (one->id() == id) return one.get();
    return nullptr;
}

const Mate* AssemblyDocument::findMateNamed(const std::string& name) const noexcept {
    for (const std::unique_ptr<Mate>& one : mates_)
        if (one->name() == name) return one.get();
    return nullptr;
}

Mate* AssemblyDocument::findMateForEdit(ObjectId id) noexcept {
    for (const std::unique_ptr<Mate>& one : mates_)
        if (one->id() == id) return one.get();
    return nullptr;
}

bool AssemblyDocument::setMateValue(ObjectId mateId, double value) {
    Mate* mate = findMateForEdit(mateId);
    if (mate == nullptr) return false;
    if (mate->type() == MateType::Fastened) return false;
    const double before = mate->value();
    if (before == value) return true;
    mate->setValue(value);
    // The FOLLOWER moves. Which end that is depends on the ground, and the
    // solve works it out -- so both ends are dirtied and the pass sorts it
    // out. Dirtying only one would be a guess about a direction that is not
    // stored anywhere (ADR-M24-002).
    graph_.markDirty(mate->leadingInstanceId());
    graph_.markDirty(mate->followingInstanceId());

    MateValueEdit edit;
    edit.mateId = mateId;
    edit.before = before;
    edit.after = value;
    recordDelta(edit, "Drive " + mate->name());
    return true;
}

// --- The solve (M24, ADR-M24-003/004) ----------------------------------------

bool AssemblyDocument::solveMates() {
    solveReport_ = MateSolveReport{};
    if (mates_.empty()) {
        // No mates: every instance is exactly where it was put, with all six
        // of its freedoms, and that is a complete and correct answer rather
        // than an unsolved one.
        for (const std::unique_ptr<PartInstance>& one : instances_) {
            const bool grounded = isInstanceGrounded(one->id());
            solveReport_.freedoms.push_back(MateSolveReport::InstanceFreedom{
                one->id(), grounded ? 0 : 3, grounded ? 0 : 3,
                grounded ? "ground" : "placed by hand"});
        }
        return true;
    }

    // WHERE EACH INSTANCE ENDS UP, worked out from the grounded ones outwards.
    //
    // This is a TREE solve and it is exact: each step places one instance from
    // one already-placed neighbour, with no iteration and no tolerance. A
    // closed loop -- a four-bar linkage -- cannot be solved this way and is
    // REFUSED by name rather than approximated, because an approximation here
    // would silently produce an assembly that does not close. That is M25's
    // work, and this is what it looks like to not have done it yet.
    std::unordered_map<ObjectId, Transform3D> placed;
    std::unordered_map<ObjectId, MateSolveReport::InstanceFreedom> freedom;
    std::vector<ObjectId> frontier;

    for (const ObjectId grounded : groundedInstances_) {
        if (findInstance(grounded) == nullptr) continue;
        placed[grounded] = instanceTransform(grounded);
        freedom[grounded] = MateSolveReport::InstanceFreedom{grounded, 0, 0, "ground"};
        frontier.push_back(grounded);
    }

    if (frontier.empty()) {
        solveReport_.ok = false;
        solveReport_.message =
            "these mates start from nowhere: nothing in this assembly is grounded, so there is "
            "no answer to where any of it goes. Ground one instance.";
        return false;
    }

    // EVERY MATE IS SPENT ONCE. Without this the walk turns round at the far
    // end of a mate and comes straight back down it, finds the instance it
    // started from already placed, and reports the loop it just made itself.
    // A mate that has been spent is the way we came; a mate that has NOT and
    // whose far end is already placed is a real closed loop.
    std::unordered_set<ObjectId> spentMates;

    while (!frontier.empty()) {
        const ObjectId from = frontier.back();
        frontier.pop_back();
        for (const std::unique_ptr<Mate>& mate : mates_) {
            const ObjectId to = mate->otherEnd(from);
            if (to == kInvalidObjectId) continue;
            if (spentMates.count(mate->id()) != 0) continue;

            const PartInstance* leader = findInstance(from);
            const PartInstance* follower = findInstance(to);
            if (leader == nullptr || follower == nullptr) continue;

            // Both connectors have to still exist on the parts. A name that no
            // longer resolves is the mate's failure, not a reason to place the
            // follower somewhere plausible.
            const PartInstance::MateConnector* leaderConnector =
                leader->findConnector(mate->connectorOn(from));
            const PartInstance::MateConnector* followerConnector =
                follower->findConnector(mate->connectorOn(to));
            if (leaderConnector == nullptr) {
                solveReport_.ok = false;
                solveReport_.message = "'" + mate->name() + "': '" + leader->name() +
                                       "' has no mate connector called '" +
                                       mate->connectorOn(from) + "'";
                return false;
            }
            if (followerConnector == nullptr) {
                solveReport_.ok = false;
                solveReport_.message = "'" + mate->name() + "': '" + follower->name() +
                                       "' has no mate connector called '" +
                                       mate->connectorOn(to) + "'";
                return false;
            }

            // THE ONE FORMULA (ADR-M24-003). Where the leader's connector is in
            // the world, then whatever freedom the mate still has, then
            // backwards from the follower's connector to the follower itself.
            const Transform3D leaderConnectorWorld =
                Compose(placed[from], leaderConnector->localTransform);
            const Transform3D wanted =
                Compose(Compose(leaderConnectorWorld, MateTransform(mate->type(), mate->value())),
                        Inverse(followerConnector->localTransform));

            const auto already = placed.find(to);
            if (already != placed.end()) {
                // Reached twice. In a tree that cannot happen, so this IS the
                // closed loop -- refused with both ends named, because the
                // reader's next move is to delete one of these mates or wait
                // for M25.
                solveReport_.ok = false;
                solveReport_.message =
                    "'" + mate->name() + "' closes a loop: '" + follower->name() +
                    "' is already placed by another chain of mates, and solving a closed loop "
                    "needs an iterative solver this does not have yet";
                return false;
            }

            spentMates.insert(mate->id());
            placed[to] = wanted;
            int rotational = 0;
            int translational = 0;
            switch (mate->type()) {
                case MateType::Fastened: break;
                case MateType::Revolute: rotational = 1; break;
                case MateType::Slider: translational = 1; break;
            }
            freedom[to] = MateSolveReport::InstanceFreedom{to, rotational, translational,
                                                           mate->name()};
            frontier.push_back(to);
        }
    }

    // Anything the mates touch but the ground does not reach has no answer.
    for (const std::unique_ptr<Mate>& mate : mates_) {
        for (const ObjectId end : {mate->leadingInstanceId(), mate->followingInstanceId()}) {
            if (placed.count(end) != 0) continue;
            solveReport_.ok = false;
            const PartInstance* stranded = findInstance(end);
            solveReport_.message =
                "'" + (stranded == nullptr ? std::string("an instance") : stranded->name()) +
                "' is mated to things that are not connected to any ground, so there is no "
                "answer to where it goes";
            return false;
        }
    }

    // APPLY. Through the frames, because that is where a placement lives --
    // and NOT recorded as undo steps: a solved position is derived, and a user
    // who presses Undo after turning a hinge means "put the angle back", not
    // "put the arm back and leave the angle".
    const bool wasApplying = applyingHistory_;
    applyingHistory_ = true;
    for (const auto& [instanceId, transform] : placed) {
        const PartInstance* instance = findInstance(instanceId);
        if (instance == nullptr) continue;
        const Transform3D current = instanceTransform(instanceId);
        if (SameTransform(current, transform)) continue;
        setFrameTransform(instance->frameId(), transform);
    }
    applyingHistory_ = wasApplying;

    for (const std::unique_ptr<PartInstance>& one : instances_) {
        const auto found = freedom.find(one->id());
        if (found != freedom.end()) {
            solveReport_.freedoms.push_back(found->second);
            continue;
        }
        // Not touched by any mate: still where it was put, still free.
        solveReport_.freedoms.push_back(
            MateSolveReport::InstanceFreedom{one->id(), 3, 3, "placed by hand"});
    }
    return true;
}

DocumentRecomputeReport AssemblyDocument::recompute() {
    // TWO PASSES, and the reason is worth stating because it looks wasteful.
    //
    // The solve needs each instance's mate connectors, and those live in the
    // PART FILE -- so they are not known until the instances have been built
    // once. The first pass builds them; the solve then moves whatever the
    // mates say to move, which dirties exactly those instances; the second
    // pass rebuilds only those.
    //
    // On an assembly that is already solved, the solve moves nothing, nothing
    // is dirtied, and the second pass is empty. The cost is paid when the
    // assembly actually changed, which is when it is worth paying.
    DocumentRecomputeReport report = DocumentBase::recompute();
    if (!report.success) return report;

    if (!solveMates()) {
        report.success = false;
        report.items.push_back({id(), RecomputeStatus::Failed, solveReport_.message});
        return report;
    }

    DocumentRecomputeReport second = DocumentBase::recompute();
    if (!second.success) return second;
    // The items of the second pass are the ones that moved; the first pass's
    // are what built. Returned together so a caller sees the whole rebuild.
    for (RecomputeItemReport& item : second.items) report.items.push_back(std::move(item));
    return report;
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
    if (const PartInstance* instance = findInstance(id)) return instance->name();
    if (const Mate* mate = findMate(id)) return mate->name();
    return {};
}

void AssemblyDocument::applyOwnName(ObjectId id, const std::string& name) {
    if (Mate* mate = findMateForEdit(id)) {
        mate->setName(name);
        return;
    }
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
    for (const std::unique_ptr<Mate>& one : mates_)
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
    if (const auto* edit = std::get_if<MateExistenceEdit>(&delta)) {
        const bool shouldExist = forward ? edit->addedByTheEdit : !edit->addedByTheEdit;
        const bool doesExist = findMate(edit->mateId) != nullptr;
        if (shouldExist == doesExist) return;
        if (shouldExist)
            restoreMate(edit->mateId, edit->name, static_cast<MateType>(edit->type),
                        edit->leadingInstanceId, edit->leadingConnector,
                        edit->followingInstanceId, edit->followingConnector, edit->value);
        else
            removeObject(edit->mateId);
        // Whatever the mate was placing has to be worked out again.
        graph_.markDirty(edit->followingInstanceId);
        return;
    }
    if (const auto* edit = std::get_if<MateValueEdit>(&delta)) {
        // Through the facade, which dirties both ends -- an undo that put the
        // number back without moving anything would leave the screen showing a
        // hinge at an angle the model no longer says it is at.
        setMateValue(edit->mateId, forward ? edit->after : edit->before);
        return;
    }
    if (const auto* edit = std::get_if<InstanceGroundEdit>(&delta)) {
        setInstanceGrounded(edit->instanceId, forward ? edit->after : edit->before);
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
    // A MATE is the simplest thing here: nothing owns it and nothing but the
    // solve reads it.
    if (const Mate* mate = findMate(id)) {
        if (!applyingHistory()) {
            MateExistenceEdit edit;
            edit.mateId = id;
            edit.name = mate->name();
            edit.type = static_cast<int>(mate->type());
            edit.leadingInstanceId = mate->leadingInstanceId();
            edit.leadingConnector = mate->leadingConnector();
            edit.followingInstanceId = mate->followingInstanceId();
            edit.followingConnector = mate->followingConnector();
            edit.value = mate->value();
            edit.addedByTheEdit = false;
            recordDelta(edit, "Delete " + mate->name());
        }
        const ObjectId follower = mate->followingInstanceId();
        graph_.removeNode(id); // a mate has none, and NodeNotFound is fine
        registry_.unregisterObject(id);
        for (auto it = mates_.begin(); it != mates_.end(); ++it)
            if ((*it)->id() == id) {
                mates_.erase(it);
                break;
            }
        // What it was placing is now placed by something else, or by nothing.
        graph_.markDirty(follower);
        return true;
    }

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
        // EVERY MATE THAT NAMED IT GOES TOO. A mate to something that is not
        // there cannot be solved and cannot be repaired -- there is no other
        // instance to re-point it at -- so leaving it would put a permanent
        // failure in the tree whose cause has already been deleted.
        std::vector<ObjectId> orphanedMates;
        for (const std::unique_ptr<Mate>& mate : mates_)
            if (mate->leadingInstanceId() == id || mate->followingInstanceId() == id)
                orphanedMates.push_back(mate->id());
        for (const ObjectId mateId : orphanedMates) removeObject(mateId);
        for (auto it = groundedInstances_.begin(); it != groundedInstances_.end(); ++it)
            if (*it == id) {
                groundedInstances_.erase(it);
                break;
            }

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
