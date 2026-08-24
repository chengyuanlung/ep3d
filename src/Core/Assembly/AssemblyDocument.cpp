#include "Core/Assembly/AssemblyDocument.h"

#include "Core/Assembly/IAssemblySolver.h"
#include "Core/Geometry/Transform.h"
#include "Core/Kernel/IGeometryKernel.h"

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

void AssemblyDocument::wireInstance(Instance& instance) {
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

Instance& AssemblyDocument::addInstance(std::string name, std::string sourcePath,
                                           std::string bodyName) {
    if (sourcePath.empty())
        throw std::runtime_error("addInstance: an instance must name a part file");
    // The frame FIRST, so the instance never exists without a place to be.
    ReferenceFrame& placement = addFrame(FrameNameFor(name), kInvalidObjectId);
    auto item = std::make_unique<Instance>(std::move(name), std::move(sourcePath),
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

Instance& AssemblyDocument::restoreInstance(ObjectId id, std::string name, ComputeState state,
                                               std::string sourcePath, std::string bodyName,
                                               ObjectId frameId) {
    requireUnusedId(id, "restoreInstance");
    if (findFrame(frameId) == nullptr)
        throw std::runtime_error("restoreInstance: frame " + std::to_string(frameId) +
                                 " is not a reference frame in this document");
    auto item = std::make_unique<Instance>(id, std::move(name), state, std::move(sourcePath),
                                              std::move(bodyName), frameId);
    auto& ref = *item;
    instances_.push_back(std::move(item));
    wireInstance(ref);
    return ref; // NOT recorded: deserialization is not a user edit (ADR-M9-001)
}

std::vector<const Instance*> AssemblyDocument::instances() const {
    std::vector<const Instance*> result;
    result.reserve(instances_.size());
    for (const std::unique_ptr<Instance>& one : instances_) result.push_back(one.get());
    return result;
}

const Instance* AssemblyDocument::findInstance(ObjectId id) const noexcept {
    for (const std::unique_ptr<Instance>& one : instances_)
        if (one->id() == id) return one.get();
    return nullptr;
}

const Instance* AssemblyDocument::findInstanceNamed(const std::string& name) const noexcept {
    for (const std::unique_ptr<Instance>& one : instances_)
        if (one->name() == name) return one.get();
    return nullptr;
}

Instance* AssemblyDocument::findInstanceForEdit(ObjectId id) noexcept {
    for (const std::unique_ptr<Instance>& one : instances_)
        if (one->id() == id) return one.get();
    return nullptr;
}

bool AssemblyDocument::setInstanceTransform(ObjectId instanceId, const Transform3D& placement) {
    const Instance* instance = findInstance(instanceId);
    if (instance == nullptr) return false;
    // Through the FRAME. Not "as well as" -- there is nowhere else a placement
    // is kept, so this cannot leave two answers behind.
    return setFrameTransform(instance->frameId(), placement);
}

Transform3D AssemblyDocument::instanceTransform(ObjectId instanceId) const noexcept {
    const Instance* instance = findInstance(instanceId);
    if (instance == nullptr) return Transform3D::Identity();
    const ReferenceFrame* frame = findFrame(instance->frameId());
    return frame == nullptr ? Transform3D::Identity() : frame->localTransform();
}

Transform3D AssemblyDocument::instanceWorldTransform(ObjectId instanceId) const noexcept {
    const Instance* instance = findInstance(instanceId);
    if (instance == nullptr) return Transform3D::Identity();
    return worldTransform(instance->frameId());
}

Transform3D AssemblyDocument::mateConnectorWorldTransform(ObjectId instanceId,
                                                          const std::string& connectorName,
                                                          bool* found) const noexcept {
    if (found != nullptr) *found = false;
    const Instance* instance = findInstance(instanceId);
    if (instance == nullptr) return Transform3D::Identity();
    const Instance::MateConnector* connector = instance->findConnector(connectorName);
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
namespace {

// A single number placed on the mate's FIRST free component -- what every
// one-freedom mate means by "the angle" or "the offset". A fastened mate has no
// free component, so a non-zero number has nowhere to go and requireMatable
// refuses it rather than dropping it here.
MateValues ValuesFromSingle(MateType type, double value) {
    MateValues values{};
    const MateFreedom freedom = FreedomOf(type);
    for (std::size_t i = 0; i < kMateComponentCount; ++i)
        if (freedom.free[i]) {
            values[i] = value;
            return values;
        }
    // NO FREE COMPONENT AT ALL -- a fastened mate. The number is put on a
    // PINNED component rather than dropped, so that requireMatable refuses it
    // with the rest. Dropping it here would make the refusal disappear the day
    // this convenience overload was written, which is exactly what happened
    // and what M24_MATE_003 caught.
    values[0] = value;
    return values;
}

} // namespace

Mate& AssemblyDocument::addMate(std::string name, MateType type, ObjectId leadingInstanceId,
                                std::string leadingConnector, ObjectId followingInstanceId,
                                std::string followingConnector, double value) {
    return addMateWithValues(std::move(name), type, leadingInstanceId,
                             std::move(leadingConnector), followingInstanceId,
                             std::move(followingConnector), ValuesFromSingle(type, value));
}

Mate& AssemblyDocument::addMateWithValues(std::string name, MateType type,
                                          ObjectId leadingInstanceId,
                                          std::string leadingConnector,
                                          ObjectId followingInstanceId,
                                          std::string followingConnector, MateValues values) {
    requireMatable(leadingInstanceId, followingInstanceId, type, values, "addMate");
    auto item = std::make_unique<Mate>(std::move(name), type, leadingInstanceId,
                                       std::move(leadingConnector), followingInstanceId,
                                       std::move(followingConnector), values);
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
    return restoreMateWithValues(id, std::move(name), type, leadingInstanceId,
                                 std::move(leadingConnector), followingInstanceId,
                                 std::move(followingConnector), ValuesFromSingle(type, value),
                                 false, std::array<Mate::Limit, kMateComponentCount>{});
}

Mate& AssemblyDocument::restoreMateWithValues(
    ObjectId id, std::string name, MateType type, ObjectId leadingInstanceId,
    std::string leadingConnector, ObjectId followingInstanceId, std::string followingConnector,
    MateValues values, bool driven,
    const std::array<Mate::Limit, kMateComponentCount>& limits) {
    requireUnusedId(id, "restoreMate");
    requireMatable(leadingInstanceId, followingInstanceId, type, values, "restoreMate");
    auto item = std::make_unique<Mate>(id, std::move(name), type, leadingInstanceId,
                                       std::move(leadingConnector), followingInstanceId,
                                       std::move(followingConnector), values);
    item->setDriven(driven);
    for (std::size_t c = 0; c < kMateComponentCount; ++c)
        item->setLimit(static_cast<int>(c), limits[c]);
    auto& ref = *item;
    mates_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    return ref; // NOT recorded: deserialization is not a user edit (ADR-M9-001)
}

void AssemblyDocument::requireMatable(ObjectId leadingInstanceId, ObjectId followingInstanceId,
                                      MateType type, const MateValues& values,
                                      const char* who) const {
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
    // A VALUE ON A COMPONENT THE MATE PINS is refused rather than ignored:
    // ignoring it would leave the writer believing they had offset something.
    // (An offset on a pinned component is a real feature -- roadmap §20.2 --
    // and it is not this one. Not done, said out loud.)
    const MateFreedom freedom = FreedomOf(type);
    for (std::size_t c = 0; c < kMateComponentCount; ++c) {
        if (freedom.free[c] || values[c] == 0.0) continue;
        throw std::runtime_error(std::string(who) + ": a " +
                                 std::string(toString(type)) +
                                 " mate has no freedom " +
                                 std::string(toString(static_cast<MateComponent>(c))) +
                                 " to give a value to");
    }
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

std::size_t AssemblyDocument::mateIndex(ObjectId mateId) const noexcept {
    for (std::size_t i = 0; i < mates_.size(); ++i)
        if (mates_[i]->id() == mateId) return i;
    return mates_.size();
}

bool AssemblyDocument::setMateValue(ObjectId mateId, double value) {
    const Mate* mate = findMate(mateId);
    if (mate == nullptr) return false;
    const int component = mate->primaryComponent();
    if (component == static_cast<int>(kMateComponentCount)) return false;
    return setMateComponentValue(mateId, static_cast<MateComponent>(component), value);
}

bool AssemblyDocument::setMateComponentValue(ObjectId mateId, MateComponent component,
                                             double value, double* clampedTo) {
    Mate* mate = findMateForEdit(mateId);
    if (mate == nullptr) return false;
    const std::size_t index = static_cast<std::size_t>(component);
    if (!mate->freedom().free[index]) return false;
    // CLAMPED, not refused (roadmap §22), and reported so it is never silent.
    const double allowed = mate->clampToLimit(static_cast<int>(index), value);
    if (clampedTo != nullptr) *clampedTo = allowed;
    MateValues before = mate->values();
    if (before[index] == allowed) return true;
    MateValues after = before;
    after[index] = allowed;
    mate->setValues(after);
    // The FOLLOWER moves. Which end that is depends on the ground, and the
    // solve works it out -- so both ends are dirtied and the pass sorts it
    // out. Dirtying only one would be a guess about a direction that is not
    // stored anywhere (ADR-M24-002).
    graph_.markDirty(mate->leadingInstanceId());
    graph_.markDirty(mate->followingInstanceId());

    MateValueEdit edit;
    edit.mateId = mateId;
    edit.component = static_cast<int>(index);
    edit.before = before[index];
    edit.after = allowed;
    recordDelta(edit, "Drive " + mate->name());
    return true;
}

bool AssemblyDocument::setMateLimit(ObjectId mateId, MateComponent component, double minimum,
                                    double maximum) {
    Mate* mate = findMateForEdit(mateId);
    if (mate == nullptr) return false;
    const std::size_t index = static_cast<std::size_t>(component);
    // A LIMIT ON SOMETHING THAT CANNOT MOVE is a control with nothing behind
    // it. Refused rather than stored where it would never be read.
    if (!mate->freedom().free[index]) return false;
    if (!(minimum <= maximum)) return false;

    const Mate::Limit before = mate->limits()[index];
    Mate::Limit after;
    after.enabled = true;
    after.min = minimum;
    after.max = maximum;
    mate->setLimit(static_cast<int>(index), after);
    // The value may now be outside its own limit. Brought inside immediately,
    // because a limit that only takes effect on the NEXT drive would let a
    // model sit in a state its own rules forbid.
    const double allowed = mate->clampToLimit(static_cast<int>(index), mate->values()[index]);
    if (allowed != mate->values()[index]) {
        MateValues values = mate->values();
        values[index] = allowed;
        mate->setValues(values);
        graph_.markDirty(mate->leadingInstanceId());
        graph_.markDirty(mate->followingInstanceId());
    }

    MateLimitEdit edit;
    edit.mateId = mateId;
    edit.component = static_cast<int>(index);
    edit.beforeEnabled = before.enabled;
    edit.beforeMin = before.min;
    edit.beforeMax = before.max;
    edit.afterEnabled = true;
    edit.afterMin = minimum;
    edit.afterMax = maximum;
    recordDelta(edit, "Limit " + mate->name());
    return true;
}

bool AssemblyDocument::clearMateLimit(ObjectId mateId, MateComponent component) {
    Mate* mate = findMateForEdit(mateId);
    if (mate == nullptr) return false;
    const std::size_t index = static_cast<std::size_t>(component);
    const Mate::Limit before = mate->limits()[index];
    if (!before.enabled) return true;
    mate->setLimit(static_cast<int>(index), Mate::Limit{});

    MateLimitEdit edit;
    edit.mateId = mateId;
    edit.component = static_cast<int>(index);
    edit.beforeEnabled = before.enabled;
    edit.beforeMin = before.min;
    edit.beforeMax = before.max;
    edit.afterEnabled = false;
    recordDelta(edit, "Unlimit " + mate->name());
    return true;
}

bool AssemblyDocument::setMateDriven(ObjectId mateId, bool driven) {
    Mate* mate = findMateForEdit(mateId);
    if (mate == nullptr) return false;
    if (mate->isDriven() == driven) return true;
    mate->setDriven(driven);
    graph_.markDirty(mate->leadingInstanceId());
    graph_.markDirty(mate->followingInstanceId());

    MateDrivenEdit edit;
    edit.mateId = mateId;
    edit.before = !driven;
    edit.after = driven;
    recordDelta(edit, (driven ? "Drive " : "Release ") + mate->name());
    return true;
}

// --- Patterns (M26, ADR-M26-003) ---------------------------------------------

std::vector<ObjectId> AssemblyDocument::addInstancePattern(ObjectId instanceId, int count,
                                                           const Vec3& step) {
    const Instance* original = findInstance(instanceId);
    if (original == nullptr)
        throw std::runtime_error("addInstancePattern: " + std::to_string(instanceId) +
                                 " is not an instance in this assembly");
    if (count < 1)
        throw std::runtime_error("addInstancePattern: a pattern needs at least one instance");

    std::vector<ObjectId> made;
    const std::string baseName = original->name();
    const std::string source = original->sourcePath();
    const std::string body = original->bodyName();
    const ObjectId originalFrame = original->frameId();

    for (int i = 1; i < count; ++i) {
        // A NAME PER COPY, numbered from the original. Not "the original plus
        // an index into a list" -- a copy is an ordinary instance and has to be
        // nameable, mateable and deletable on its own.
        std::string copyName = baseName + " " + std::to_string(i + 1);
        for (int attempt = 2; findInstanceNamed(copyName) != nullptr; ++attempt)
            copyName = baseName + " " + std::to_string(i + 1) + "." + std::to_string(attempt);

        Instance& copy = addInstance(copyName, source, body);
        // THE COPY'S FRAME HANGS OFF THE ORIGINAL'S. That is what makes the
        // pattern parametric: move the original and every copy follows,
        // because the frame hierarchy already composes (ADR-M10-002) and
        // nothing here watches anything.
        setFrameParent(copy.frameId(), originalFrame);
        Transform3D offset;
        offset.translation = Vec3{step.x * i, step.y * i, step.z * i};
        setFrameTransform(copy.frameId(), offset);
        made.push_back(copy.id());
    }
    return made;
}

// --- Named positions (M26, roadmap §49) --------------------------------------

NamedPosition& AssemblyDocument::captureNamedPosition(std::string name) {
    // EVERY MATE'S FREEDOMS, and every instance no mate places. The second
    // half is easy to leave out and impossible to notice until an assembly
    // with a loose part comes back with it somewhere else.
    std::vector<NamedPosition::MateSetting> mates;
    mates.reserve(mates_.size());
    for (const std::unique_ptr<Mate>& mate : mates_)
        mates.push_back(NamedPosition::MateSetting{mate->id(), mate->values()});

    std::unordered_set<ObjectId> placedByAMate;
    for (const std::unique_ptr<Mate>& mate : mates_) {
        placedByAMate.insert(mate->leadingInstanceId());
        placedByAMate.insert(mate->followingInstanceId());
    }
    std::vector<NamedPosition::LooseSetting> loose;
    for (const std::unique_ptr<Instance>& one : instances_) {
        if (placedByAMate.count(one->id()) != 0) continue;
        loose.push_back(NamedPosition::LooseSetting{one->id(), instanceTransform(one->id())});
    }

    auto item = std::make_unique<NamedPosition>(std::move(name), std::move(mates),
                                                std::move(loose));
    auto& ref = *item;
    namedPositions_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);

    NamedPositionExistenceEdit edit;
    edit.positionId = ref.id();
    edit.name = ref.name();
    edit.mates = ref.mates();
    edit.loose = ref.loose();
    edit.addedByTheEdit = true;
    recordDelta(edit, "Capture " + ref.name());
    return ref;
}

NamedPosition& AssemblyDocument::restoreNamedPosition(
    ObjectId id, std::string name, std::vector<NamedPosition::MateSetting> mates,
    std::vector<NamedPosition::LooseSetting> loose) {
    requireUnusedId(id, "restoreNamedPosition");
    auto item = std::make_unique<NamedPosition>(id, std::move(name), std::move(mates),
                                                std::move(loose));
    auto& ref = *item;
    namedPositions_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    return ref; // NOT recorded: deserialization is not a user edit (ADR-M9-001)
}

bool AssemblyDocument::applyNamedPosition(ObjectId positionId) {
    const NamedPosition* pose = findNamedPosition(positionId);
    if (pose == nullptr) return false;

    // ONE UNDO STEP, because a pose is one thing the user chose. Without the
    // transaction, undoing "go to Open" would walk backwards through every
    // mate it touched, one press at a time.
    const bool nested = isTransactionOpen();
    if (!nested) beginTransaction("Apply " + pose->name());
    for (const NamedPosition::MateSetting& setting : pose->mates()) {
        Mate* mate = findMateForEdit(setting.mateId);
        if (mate == nullptr) continue; // the mate is gone; the rest still applies
        const MateFreedom freedom = mate->freedom();
        for (std::size_t c = 0; c < kMateComponentCount; ++c) {
            if (!freedom.free[c]) continue;
            setMateComponentValue(setting.mateId, static_cast<MateComponent>(c),
                                  setting.values[c]);
        }
    }
    for (const NamedPosition::LooseSetting& setting : pose->loose())
        setInstanceTransform(setting.instanceId, setting.transform);
    if (!nested) commitTransaction();
    return true;
}

std::vector<const NamedPosition*> AssemblyDocument::namedPositions() const {
    std::vector<const NamedPosition*> out;
    out.reserve(namedPositions_.size());
    for (const std::unique_ptr<NamedPosition>& one : namedPositions_) out.push_back(one.get());
    return out;
}

const NamedPosition* AssemblyDocument::findNamedPosition(ObjectId id) const noexcept {
    for (const std::unique_ptr<NamedPosition>& one : namedPositions_)
        if (one->id() == id) return one.get();
    return nullptr;
}

const NamedPosition* AssemblyDocument::findNamedPositionNamed(
    const std::string& name) const noexcept {
    for (const std::unique_ptr<NamedPosition>& one : namedPositions_)
        if (one->name() == name) return one.get();
    return nullptr;
}

NamedPosition* AssemblyDocument::findNamedPositionForEdit(ObjectId id) noexcept {
    for (const std::unique_ptr<NamedPosition>& one : namedPositions_)
        if (one->id() == id) return one.get();
    return nullptr;
}

// --- Exploded views (M26, roadmap §49) ---------------------------------------

ExplodeView& AssemblyDocument::addExplodeView(std::string name) {
    auto item = std::make_unique<ExplodeView>(std::move(name), std::vector<ExplodeStep>{});
    auto& ref = *item;
    explodeViews_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);

    ExplodeViewExistenceEdit edit;
    edit.viewId = ref.id();
    edit.name = ref.name();
    edit.addedByTheEdit = true;
    recordDelta(edit, "Add " + ref.name());
    return ref;
}

ExplodeView& AssemblyDocument::restoreExplodeView(ObjectId id, std::string name,
                                                  std::vector<ExplodeStep> steps,
                                                  std::size_t previewCut) {
    requireUnusedId(id, "restoreExplodeView");
    auto item = std::make_unique<ExplodeView>(id, std::move(name), std::move(steps), previewCut);
    auto& ref = *item;
    explodeViews_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    return ref;
}

bool AssemblyDocument::addExplodeStep(ObjectId viewId, std::string stepName, ObjectId instanceId,
                                      const Vec3& offset) {
    ExplodeView* view = findExplodeViewForEdit(viewId);
    if (view == nullptr) return false;
    if (findInstance(instanceId) == nullptr) return false;

    std::vector<ExplodeStep> steps = view->steps();
    const std::vector<ExplodeStep> before = steps;
    ExplodeStep step;
    step.name = std::move(stepName);
    step.instanceId = instanceId;
    step.displacement.translation = offset;
    steps.push_back(std::move(step));
    view->setSteps(steps);

    ExplodeStepsEdit edit;
    edit.viewId = viewId;
    edit.before = before;
    edit.after = steps;
    recordDelta(edit, "Explode " + objectName(instanceId));
    return true;
}

bool AssemblyDocument::moveExplodeStep(ObjectId viewId, std::size_t from, std::size_t to) {
    ExplodeView* view = findExplodeViewForEdit(viewId);
    if (view == nullptr) return false;
    std::vector<ExplodeStep> steps = view->steps();
    if (from >= steps.size() || to >= steps.size()) return false;
    if (from == to) return true;
    const std::vector<ExplodeStep> before = steps;
    ExplodeStep moved = steps[from];
    steps.erase(steps.begin() + static_cast<std::ptrdiff_t>(from));
    steps.insert(steps.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
    view->setSteps(steps);

    ExplodeStepsEdit edit;
    edit.viewId = viewId;
    edit.before = before;
    edit.after = steps;
    recordDelta(edit, "Reorder " + view->name());
    return true;
}

bool AssemblyDocument::removeExplodeStep(ObjectId viewId, std::size_t index) {
    ExplodeView* view = findExplodeViewForEdit(viewId);
    if (view == nullptr) return false;
    std::vector<ExplodeStep> steps = view->steps();
    if (index >= steps.size()) return false;
    const std::vector<ExplodeStep> before = steps;
    steps.erase(steps.begin() + static_cast<std::ptrdiff_t>(index));
    view->setSteps(steps);

    ExplodeStepsEdit edit;
    edit.viewId = viewId;
    edit.before = before;
    edit.after = steps;
    recordDelta(edit, "Delete a step of " + view->name());
    return true;
}

bool AssemblyDocument::setExplodePreview(ObjectId viewId, std::size_t stepsShown) {
    ExplodeView* view = findExplodeViewForEdit(viewId);
    if (view == nullptr) return false;
    const std::size_t before = view->previewCut();
    if (before == stepsShown) return true;
    view->setPreviewCut(stepsShown);

    ExplodePreviewEdit edit;
    edit.viewId = viewId;
    edit.before = before;
    edit.after = stepsShown;
    recordDelta(edit, "Preview " + view->name());
    return true;
}

std::vector<const ExplodeView*> AssemblyDocument::explodeViews() const {
    std::vector<const ExplodeView*> out;
    out.reserve(explodeViews_.size());
    for (const std::unique_ptr<ExplodeView>& one : explodeViews_) out.push_back(one.get());
    return out;
}

const ExplodeView* AssemblyDocument::findExplodeView(ObjectId id) const noexcept {
    for (const std::unique_ptr<ExplodeView>& one : explodeViews_)
        if (one->id() == id) return one.get();
    return nullptr;
}

const ExplodeView* AssemblyDocument::findExplodeViewNamed(const std::string& name) const noexcept {
    for (const std::unique_ptr<ExplodeView>& one : explodeViews_)
        if (one->name() == name) return one.get();
    return nullptr;
}

ExplodeView* AssemblyDocument::findExplodeViewForEdit(ObjectId id) noexcept {
    for (const std::unique_ptr<ExplodeView>& one : explodeViews_)
        if (one->id() == id) return one.get();
    return nullptr;
}

Transform3D AssemblyDocument::explodedWorldTransform(ObjectId viewId,
                                                     ObjectId instanceId) const noexcept {
    const Transform3D placed = instanceWorldTransform(instanceId);
    const ExplodeView* view = findExplodeView(viewId);
    // NO VIEW MEANS NO EXPLOSION, and that is the assembly's own answer
    // unchanged -- which is the evidence that an explosion is a picture rather
    // than a move.
    if (view == nullptr) return placed;
    return Compose(placed, view->displacementOf(instanceId));
}

// --- The solve (M24, ADR-M24-003/004) ----------------------------------------

// THE SPANNING FOREST, BUILT ONCE (M25, ADR-M25-002).
//
// Which mate places which instance, walking outwards from the grounded ones.
// M24 did this and placed as it went; M25 separates the two, because a closed
// loop has to place EVERYTHING many times with different mate values, and the
// shape of the walk does not change between attempts.
//
// The mates NOT in the forest are the ones that close loops. In M24 there could
// be none -- one was a refusal. Now they are the equations.
bool AssemblyDocument::buildMateForest(MateForest& forest) const {
    forest = MateForest{};
    for (const ObjectId grounded : groundedInstances_) {
        if (findInstance(grounded) == nullptr) continue;
        forest.roots.push_back(grounded);
        forest.reached.insert(grounded);
    }
    if (forest.roots.empty() && !mates_.empty()) {
        forest.message =
            "these mates start from nowhere: nothing in this assembly is grounded, so there is "
            "no answer to where any of it goes. Ground one instance.";
        return false;
    }

    std::vector<ObjectId> frontier = forest.roots;
    // EVERY MATE IS SPENT ONCE. Without this the walk turns round at the far
    // end of a mate and comes straight back down it, finds the instance it
    // started from already reached, and calls the way it came a loop.
    std::unordered_set<ObjectId> spent;
    while (!frontier.empty()) {
        const ObjectId from = frontier.back();
        frontier.pop_back();
        for (const std::unique_ptr<Mate>& mate : mates_) {
            const ObjectId to = mate->otherEnd(from);
            if (to == kInvalidObjectId) continue;
            if (spent.count(mate->id()) != 0) continue;
            if (findInstance(to) == nullptr) continue;
            if (forest.reached.count(to) != 0) continue; // a loop closer; kept below
            spent.insert(mate->id());
            forest.reached.insert(to);
            forest.steps.push_back(MateForest::Step{mate->id(), from, to});
            frontier.push_back(to);
        }
    }
    for (const std::unique_ptr<Mate>& mate : mates_) {
        if (spent.count(mate->id()) != 0) continue;
        // Both ends reached, but this mate was not the one that reached
        // either: it says something ELSE about a pair the forest already
        // placed. That is a closed loop, and it is now an equation rather
        // than a refusal (M24 could only refuse).
        if (forest.reached.count(mate->leadingInstanceId()) != 0 &&
            forest.reached.count(mate->followingInstanceId()) != 0)
            forest.loopClosers.push_back(mate->id());
    }

    // Anything a mate touches that the ground does not reach has no answer.
    for (const std::unique_ptr<Mate>& mate : mates_) {
        for (const ObjectId end : {mate->leadingInstanceId(), mate->followingInstanceId()}) {
            if (forest.reached.count(end) != 0) continue;
            const Instance* stranded = findInstance(end);
            forest.message =
                "'" + (stranded == nullptr ? std::string("an instance") : stranded->name()) +
                "' is mated to things that are not connected to any ground, so there is no "
                "answer to where it goes";
            return false;
        }
    }
    return true;
}

// WHERE EVERYTHING ENDS UP, given a value for every mate.
//
// Exact: each step places one instance from one already-placed neighbour, with
// no iteration and no tolerance. Called once for a tree assembly, and once per
// solver probe for a mechanism.
bool AssemblyDocument::placeThroughForest(const MateForest& forest,
                                          const std::vector<MateValues>& mateValues,
                                          std::unordered_map<ObjectId, Transform3D>& placed,
                                          std::string* whyNot) const {
    placed.clear();
    for (const ObjectId root : forest.roots) placed[root] = instanceTransform(root);

    for (const MateForest::Step& step : forest.steps) {
        const Mate* mate = findMate(step.mateId);
        const Instance* leader = findInstance(step.from);
        const Instance* follower = findInstance(step.to);
        if (mate == nullptr || leader == nullptr || follower == nullptr) continue;

        // Both connectors have to still exist on the parts. A name that no
        // longer resolves is the mate's failure, not a reason to place the
        // follower somewhere plausible.
        const Instance::MateConnector* leaderConnector =
            leader->findConnector(mate->connectorOn(step.from));
        const Instance::MateConnector* followerConnector =
            follower->findConnector(mate->connectorOn(step.to));
        if (leaderConnector == nullptr) {
            if (whyNot != nullptr)
                *whyNot = "'" + mate->name() + "': '" + leader->name() +
                          "' has no mate connector called '" + mate->connectorOn(step.from) + "'";
            return false;
        }
        if (followerConnector == nullptr) {
            if (whyNot != nullptr)
                *whyNot = "'" + mate->name() + "': '" + follower->name() +
                          "' has no mate connector called '" + mate->connectorOn(step.to) + "'";
            return false;
        }

        // THE ONE FORMULA (ADR-M24-003). Where the leader's connector is in the
        // world, then whatever freedom the mate still has, then backwards from
        // the follower's connector to the follower itself.
        //
        // A mate can be walked from EITHER end -- the ground decides which --
        // and the middle transform is stated from the leading end, so walking
        // it backwards means inverting it. Getting this wrong would place a
        // hinge at minus its angle whenever the chain happened to run the
        // other way, which is a defect that only shows up in some assemblies.
        const bool forwards = step.from == mate->leadingInstanceId();
        const Transform3D middle = MateTransform(mate->type(), mateValues[mateIndex(step.mateId)]);
        const Transform3D leaderConnectorWorld =
            Compose(placed[step.from], leaderConnector->localTransform);
        placed[step.to] = Compose(Compose(leaderConnectorWorld, forwards ? middle
                                                                         : Inverse(middle)),
                                  Inverse(followerConnector->localTransform));
    }
    return true;
}

// HOW FAR THE LOOP-CLOSING MATES ARE FROM HOLDING.
//
// One residual per component each of them PINS. Zero everywhere means the
// mechanism closes.
bool AssemblyDocument::loopResiduals(const MateForest& forest,
                                     const std::vector<MateValues>& mateValues,
                                     const std::unordered_map<ObjectId, Transform3D>& placed,
                                     double* out, std::size_t* count) const {
    std::size_t written = 0;
    for (const ObjectId mateId : forest.loopClosers) {
        const Mate* mate = findMate(mateId);
        if (mate == nullptr) continue;
        const Instance* leader = findInstance(mate->leadingInstanceId());
        const Instance* follower = findInstance(mate->followingInstanceId());
        if (leader == nullptr || follower == nullptr) return false;
        const Instance::MateConnector* leaderConnector =
            leader->findConnector(mate->leadingConnector());
        const Instance::MateConnector* followerConnector =
            follower->findConnector(mate->followingConnector());
        if (leaderConnector == nullptr || followerConnector == nullptr) return false;

        const auto leaderPlaced = placed.find(mate->leadingInstanceId());
        const auto followerPlaced = placed.find(mate->followingInstanceId());
        if (leaderPlaced == placed.end() || followerPlaced == placed.end()) return false;

        // Where the follower's connector actually sits, seen from the leader's.
        const Transform3D relative =
            Compose(Inverse(Compose(leaderPlaced->second, leaderConnector->localTransform)),
                    Compose(followerPlaced->second, followerConnector->localTransform));
        // A DRIVEN mate pins its freedoms too. Otherwise a driven slider that
        // happened to be the mate closing the loop would absorb the whole
        // mismatch in its own travel, report success, and overwrite the number
        // the user typed with whatever the slack turned out to be.
        written += static_cast<std::size_t>(MateResiduals(mate->type(),
                                                          mateValues[mateIndex(mateId)], relative,
                                                          out + written, mate->isDriven()));
    }
    if (count != nullptr) *count = written;
    return true;
}

bool AssemblyDocument::solveMates() {
    solveReport_ = MateSolveReport{};
    if (mates_.empty()) {
        // No mates: every instance is exactly where it was put, with all six
        // of its freedoms, and that is a complete and correct answer rather
        // than an unsolved one.
        for (const std::unique_ptr<Instance>& one : instances_) {
            const bool grounded = isInstanceGrounded(one->id());
            solveReport_.freedoms.push_back(MateSolveReport::InstanceFreedom{
                one->id(), grounded ? 0 : 3, grounded ? 0 : 3,
                grounded ? "ground" : "placed by hand"});
        }
        return true;
    }

    MateForest forest;
    if (!buildMateForest(forest)) {
        solveReport_.ok = false;
        solveReport_.message = forest.message;
        return false;
    }

    // The values every mate currently holds, indexed the same way mates_ is.
    std::vector<MateValues> mateValues;
    mateValues.reserve(mates_.size());
    for (const std::unique_ptr<Mate>& mate : mates_) mateValues.push_back(mate->values());

    // WHICH NUMBERS THE SOLVE MAY CHOOSE.
    //
    // Only the free components of mates that PLACE something (the forest's
    // steps) and are not driven. A loop-closing mate's own freedom moves
    // nothing, so it is not an unknown -- it is read back afterwards from
    // where the loop actually ended up.
    struct Unknown {
        std::size_t mateIndex;
        std::size_t component;
    };
    std::vector<Unknown> unknowns;
    if (!forest.loopClosers.empty()) {
        for (const MateForest::Step& step : forest.steps) {
            const Mate* mate = findMate(step.mateId);
            if (mate == nullptr || mate->isDriven()) continue;
            const MateFreedom freedom = mate->freedom();
            for (std::size_t c = 0; c < kMateComponentCount; ++c)
                if (freedom.free[c]) unknowns.push_back(Unknown{mateIndex(step.mateId), c});
        }
    }

    std::unordered_map<ObjectId, Transform3D> placed;
    std::string whyNot;

    if (forest.loopClosers.empty()) {
        // A TREE. Exact, one pass, no solver -- which is what almost every
        // assembly is, and what M24 shipped.
        if (!placeThroughForest(forest, mateValues, placed, &whyNot)) {
            solveReport_.ok = false;
            solveReport_.message = whyNot;
            return false;
        }
    } else {
        // A MECHANISM. The loop-closing mates are equations, the undriven
        // freedoms along the tree are the unknowns, and the seed is where
        // everything currently sits -- which is not a convenience: it is what
        // picks WHICH of a mechanism's several valid configurations comes
        // back. A four-bar has two for most crank angles, and jumping between
        // them between frames is the classic assembly-solver misbehaviour.
        if (assemblySolver_ == nullptr) {
            solveReport_.ok = false;
            solveReport_.message =
                "these mates form a closed loop and no assembly solver is configured";
            return false;
        }

        // How many equations, asked once at the current configuration so a
        // failure to resolve a connector is reported here rather than from
        // inside the solver's callback where there is nothing to say it with.
        if (!placeThroughForest(forest, mateValues, placed, &whyNot)) {
            solveReport_.ok = false;
            solveReport_.message = whyNot;
            return false;
        }
        std::array<double, kMateComponentCount * 64> scratch{};
        std::size_t residualCount = 0;
        if (!loopResiduals(forest, mateValues, placed, scratch.data(), &residualCount)) {
            solveReport_.ok = false;
            solveReport_.message = "a mate in the loop names a connector that does not resolve";
            return false;
        }

        AssemblySolveProblem problem;
        problem.residualCount = residualCount;
        problem.initial.reserve(unknowns.size());
        for (const Unknown& unknown : unknowns)
            problem.initial.push_back(mateValues[unknown.mateIndex][unknown.component]);

        // The callback owns copies, so a probe cannot leave the document's
        // mate values disturbed if the solve gives up half way.
        std::vector<MateValues> probeValues = mateValues;
        problem.evaluate = [&](const double* x, double* residuals) {
            for (std::size_t i = 0; i < unknowns.size(); ++i)
                probeValues[unknowns[i].mateIndex][unknowns[i].component] = x[i];
            std::unordered_map<ObjectId, Transform3D> probePlaced;
            if (!placeThroughForest(forest, probeValues, probePlaced, nullptr)) {
                for (std::size_t i = 0; i < residualCount; ++i) residuals[i] = 0.0;
                return;
            }
            std::size_t written = 0;
            loopResiduals(forest, probeValues, probePlaced, residuals, &written);
            for (std::size_t i = written; i < residualCount; ++i) residuals[i] = 0.0;
        };

        // WHERE THE SEARCH STARTS, AND WHY IT SOMETIMES HAS TO START AGAIN.
        //
        // The seed is the assembly's current configuration, and that is the
        // right first guess: a mechanism being dragged is already nearly
        // solved, and starting from where it is picks the configuration
        // NEAREST to the one on screen. A four-bar has two solutions for most
        // crank angles, and jumping between them between frames is the classic
        // assembly-solver misbehaviour.
        //
        // But a freshly-mated linkage starts with every angle at zero, which
        // puts all its links collinear -- a DEAD POINT, where the Jacobian
        // loses rank and the search has no downhill direction to take. That is
        // not a mechanism that cannot close; it is a start that cannot move.
        //
        // So a failed solve is retried from a few deterministic offsets. Not
        // random ones: a solver whose answer depends on a random seed gives a
        // different assembly on different runs of the same file, and this
        // project has no place for that. The offsets fan out because a dead
        // point is escaped by leaving it, and which direction does not matter.
        static constexpr double kRestarts[] = {0.0, 0.35, -0.35, 1.1, -1.1, 2.2};
        AssemblySolveResult solved;
        bool anySolved = false;
        for (const double offset : kRestarts) {
            if (offset != 0.0) {
                for (std::size_t i = 0; i < unknowns.size(); ++i) {
                    // ALTERNATING, so the restarts explore shapes rather than
                    // sliding the whole linkage along one direction -- which
                    // for a loop is a move that changes nothing.
                    const double sign = (i % 2 == 0) ? 1.0 : -1.0;
                    problem.initial[i] =
                        mateValues[unknowns[i].mateIndex][unknowns[i].component] + sign * offset;
                }
            }
            solved = assemblySolver_->solve(problem);
            if (solved) {
                anySolved = true;
                break;
            }
        }
        if (!anySolved) {
            solveReport_.ok = false;
            solveReport_.message =
                std::string("this mechanism does not close: the solve ") +
                std::string(toString(solved.status)) +
                (solved.message.empty() ? "" : " -- " + solved.message);
            return false;
        }
        for (std::size_t i = 0; i < unknowns.size(); ++i)
            mateValues[unknowns[i].mateIndex][unknowns[i].component] = solved.values[i];
        if (!placeThroughForest(forest, mateValues, placed, &whyNot)) {
            solveReport_.ok = false;
            solveReport_.message = whyNot;
            return false;
        }
        solveReport_.mechanismDegreesOfFreedom = solved.degreesOfFreedom;
        solveReport_.iterations = solved.iterations;

        // WRITE THE SOLVED ANGLES BACK. The model has to say where things are:
        // a hinge whose stored angle disagreed with the arm on screen would be
        // two answers to one question, which is the thing this project spends
        // its milestones removing. The sketch solver does exactly this with
        // geometry, for exactly this reason.
        const bool wasApplying = applyingHistory_;
        applyingHistory_ = true;
        for (const Unknown& unknown : unknowns)
            mates_[unknown.mateIndex]->setValues(mateValues[unknown.mateIndex]);
        // ...and the loop closers, read back from where the loop actually
        // ended up, so THEIR angles are true as well.
        for (const ObjectId mateId : forest.loopClosers) {
            Mate* mate = findMateForEdit(mateId);
            if (mate == nullptr) continue;
            // A DRIVEN mate says where it is; the solve does not get to
            // re-decide that, whichever side of the spanning tree it fell on.
            if (mate->isDriven()) continue;
            const Instance* leader = findInstance(mate->leadingInstanceId());
            const Instance* follower = findInstance(mate->followingInstanceId());
            if (leader == nullptr || follower == nullptr) continue;
            const Instance::MateConnector* leaderConnector =
                leader->findConnector(mate->leadingConnector());
            const Instance::MateConnector* followerConnector =
                follower->findConnector(mate->followingConnector());
            if (leaderConnector == nullptr || followerConnector == nullptr) continue;
            const Transform3D relative = Compose(
                Inverse(Compose(placed[mate->leadingInstanceId()], leaderConnector->localTransform)),
                Compose(placed[mate->followingInstanceId()], followerConnector->localTransform));
            const std::array<double, kMateComponentCount> achieved = ComponentsOf(relative);
            MateValues values = mate->values();
            const MateFreedom freedom = mate->freedom();
            for (std::size_t c = 0; c < kMateComponentCount; ++c)
                if (freedom.free[c]) values[c] = achieved[c];
            mate->setValues(values);
        }
        applyingHistory_ = wasApplying;
    }

    // --- what the solve concluded, per instance ------------------------------
    std::unordered_map<ObjectId, MateSolveReport::InstanceFreedom> freedom;
    for (const ObjectId root : forest.roots)
        freedom[root] = MateSolveReport::InstanceFreedom{root, 0, 0, "ground"};
    for (const MateForest::Step& step : forest.steps) {
        const Mate* mate = findMate(step.mateId);
        if (mate == nullptr) continue;
        const MateFreedom left = mate->freedom();
        freedom[step.to] = MateSolveReport::InstanceFreedom{
            step.to, left.rotational(), left.translational(), mate->name()};
    }
    // INSIDE A CLOSED LOOP, per-instance freedom is not a meaningful thing to
    // report and saying it anyway would be worse than saying nothing: the
    // freedom belongs to the MECHANISM, not to any one part of it. A four-bar
    // whose three moving links each "have one rotation" reads as three
    // freedoms when the linkage has one. So the loop's members are marked as
    // such and the number is reported once, for the mechanism.
    if (!forest.loopClosers.empty()) {
        std::unordered_set<ObjectId> inLoop;
        for (const ObjectId mateId : forest.loopClosers) {
            const Mate* mate = findMate(mateId);
            if (mate == nullptr) continue;
            inLoop.insert(mate->leadingInstanceId());
            inLoop.insert(mate->followingInstanceId());
        }
        for (const MateForest::Step& step : forest.steps) inLoop.insert(step.to);
        for (const ObjectId id : inLoop) {
            if (isInstanceGrounded(id)) continue;
            auto found = freedom.find(id);
            if (found == freedom.end()) continue;
            found->second.rotational = 0;
            found->second.translational = 0;
            found->second.describedBy = "in a closed loop";
        }
    }

    // APPLY. Through the frames, because that is where a placement lives --
    // and NOT recorded as undo steps: a solved position is derived, and a user
    // who presses Undo after turning a hinge means "put the angle back", not
    // "put the arm back and leave the angle".
    const bool wasApplying = applyingHistory_;
    applyingHistory_ = true;
    for (const auto& [instanceId, transform] : placed) {
        const Instance* instance = findInstance(instanceId);
        if (instance == nullptr) continue;
        const Transform3D current = instanceTransform(instanceId);
        if (SameTransform(current, transform)) continue;
        setFrameTransform(instance->frameId(), transform);
    }
    applyingHistory_ = wasApplying;

    for (const std::unique_ptr<Instance>& one : instances_) {
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

AssemblyDocument::InterferenceReport AssemblyDocument::checkInterference() const {
    InterferenceReport report;
    IGeometryKernel* kernel = geometryKernel();
    if (kernel == nullptr) {
        report.ok = false;
        report.message = "no geometry kernel configured";
        return report;
    }

    // WHAT IS ACTUALLY THERE. An instance that has not been built is skipped
    // and SAID SO -- reporting it as clear would be the same sentence as "no
    // interference", which is the one thing this must never say by accident.
    std::vector<const Instance*> built;
    std::vector<KernelBoundsResult> bounds;
    int skipped = 0;
    for (const std::unique_ptr<Instance>& one : instances_) {
        if (one->currentState() != ComputeState::Valid || !one->currentShape().isValid()) {
            ++skipped;
            continue;
        }
        const KernelBoundsResult box = kernel->boundsOfShape(one->currentShape());
        if (!box.ok) {
            ++skipped;
            continue;
        }
        built.push_back(one.get());
        bounds.push_back(box);
    }
    if (skipped > 0) {
        report.ok = false;
        report.message = std::to_string(skipped) +
                         " instance(s) have not been built, so this is not a full answer -- "
                         "solve first";
    }

    for (std::size_t i = 0; i < built.size(); ++i) {
        for (std::size_t j = i + 1; j < built.size(); ++j) {
            // BROAD PHASE (roadmap §23): boxes that do not overlap cannot,
            // and this is the difference between an assembly that can be
            // checked while it moves and one that cannot. The tolerance is
            // one-sided on purpose -- boxes that merely touch are let
            // through to the precise phase, which is the cheap direction to
            // be wrong in.
            const KernelBoundsResult& a = bounds[i];
            const KernelBoundsResult& b = bounds[j];
            if (a.max.x < b.min.x || b.max.x < a.min.x) continue;
            if (a.max.y < b.min.y || b.max.y < a.min.y) continue;
            if (a.max.z < b.min.z || b.max.z < a.min.z) continue;

            const KernelInterferenceResult measured =
                kernel->measureInterference(built[i]->currentShape(), built[j]->currentShape());
            if (!measured) {
                report.ok = false;
                report.message = "'" + built[i]->name() + "' and '" + built[j]->name() +
                                 "' could not be compared: " + measured.message;
                continue;
            }
            if (measured.volumeMm3 <= 0.0) continue;
            report.overlaps.push_back(
                Interference{built[i]->id(), built[j]->id(), measured.volumeMm3});
        }
    }
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
    if (const Instance* instance = findInstance(id)) return instance->name();
    if (const Mate* mate = findMate(id)) return mate->name();
    if (const NamedPosition* pose = findNamedPosition(id)) return pose->name();
    if (const ExplodeView* view = findExplodeView(id)) return view->name();
    return {};
}

void AssemblyDocument::applyOwnName(ObjectId id, const std::string& name) {
    if (Mate* mate = findMateForEdit(id)) {
        mate->setName(name);
        return;
    }
    if (NamedPosition* pose = findNamedPositionForEdit(id)) {
        pose->setName(name);
        return;
    }
    if (ExplodeView* view = findExplodeViewForEdit(id)) {
        view->setName(name);
        return;
    }
    Instance* instance = findInstanceForEdit(id);
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
    for (const std::unique_ptr<Instance>& one : instances_)
        if (one->id() != except && one->name() == name) return true;
    for (const std::unique_ptr<Mate>& one : mates_)
        if (one->id() != except && one->name() == name) return true;
    for (const std::unique_ptr<NamedPosition>& one : namedPositions_)
        if (one->id() != except && one->name() == name) return true;
    for (const std::unique_ptr<ExplodeView>& one : explodeViews_)
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
    if (const auto* edit = std::get_if<NamedPositionExistenceEdit>(&delta)) {
        const bool shouldExist = forward ? edit->addedByTheEdit : !edit->addedByTheEdit;
        const bool doesExist = findNamedPosition(edit->positionId) != nullptr;
        if (shouldExist == doesExist) return;
        if (shouldExist)
            restoreNamedPosition(edit->positionId, edit->name, edit->mates, edit->loose);
        else
            removeObject(edit->positionId);
        return;
    }
    if (const auto* edit = std::get_if<ExplodeViewExistenceEdit>(&delta)) {
        const bool shouldExist = forward ? edit->addedByTheEdit : !edit->addedByTheEdit;
        const bool doesExist = findExplodeView(edit->viewId) != nullptr;
        if (shouldExist == doesExist) return;
        if (shouldExist)
            restoreExplodeView(edit->viewId, edit->name, edit->steps, edit->previewCut);
        else
            removeObject(edit->viewId);
        return;
    }
    if (const auto* edit = std::get_if<ExplodeStepsEdit>(&delta)) {
        ExplodeView* view = findExplodeViewForEdit(edit->viewId);
        if (view != nullptr) view->setSteps(forward ? edit->after : edit->before);
        return;
    }
    if (const auto* edit = std::get_if<ExplodePreviewEdit>(&delta)) {
        ExplodeView* view = findExplodeViewForEdit(edit->viewId);
        if (view != nullptr) view->setPreviewCut(forward ? edit->after : edit->before);
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

    // A POSE and an EXPLODED VIEW are the simplest things here: nothing owns
    // them, nothing else reads them, and neither holds geometry.
    if (const NamedPosition* pose = findNamedPosition(id)) {
        if (!applyingHistory()) {
            NamedPositionExistenceEdit edit;
            edit.positionId = id;
            edit.name = pose->name();
            edit.mates = pose->mates();
            edit.loose = pose->loose();
            edit.addedByTheEdit = false;
            recordDelta(edit, "Delete " + pose->name());
        }
        registry_.unregisterObject(id);
        for (auto it = namedPositions_.begin(); it != namedPositions_.end(); ++it)
            if ((*it)->id() == id) {
                namedPositions_.erase(it);
                break;
            }
        return true;
    }
    if (const ExplodeView* view = findExplodeView(id)) {
        if (!applyingHistory()) {
            ExplodeViewExistenceEdit edit;
            edit.viewId = id;
            edit.name = view->name();
            edit.steps = view->steps();
            edit.previewCut = view->previewCut();
            edit.addedByTheEdit = false;
            recordDelta(edit, "Delete " + view->name());
        }
        registry_.unregisterObject(id);
        for (auto it = explodeViews_.begin(); it != explodeViews_.end(); ++it)
            if ((*it)->id() == id) {
                explodeViews_.erase(it);
                break;
            }
        return true;
    }

    const Instance* instance = findInstance(id);
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
