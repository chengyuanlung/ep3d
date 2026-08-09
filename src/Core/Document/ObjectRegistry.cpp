#include "Core/Document/ObjectRegistry.h"
#include "Core/Body/Body.h"
#include "Core/Feature/Feature.h"
#include "Core/Material/Material.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Recompute/IRecomputable.h"
#include "Core/Sketch/Sketch.h"

namespace paramcad {

namespace {

// Id carried by the handle itself; kInvalidObjectId for null handles.
ObjectId handleId(const ObjectRegistry::ObjectRef& ref) {
    return std::visit(
        [](auto* object) -> ObjectId {
            return object != nullptr ? object->id() : kInvalidObjectId;
        },
        ref);
}

} // namespace

bool ObjectRegistry::registerObject(ObjectId id, ObjectRef ref) {
    if (id == kInvalidObjectId) return false;
    if (handleId(ref) != id) return false; // null handle or mismatched identity
    return objects_.emplace(id, ref).second; // duplicate id rejected
}

bool ObjectRegistry::unregisterObject(ObjectId id) noexcept {
    return objects_.erase(id) != 0;
}

bool ObjectRegistry::contains(ObjectId id) const noexcept {
    return objects_.count(id) != 0;
}

const ObjectRegistry::ObjectRef* ObjectRegistry::find(ObjectId id) const noexcept {
    const auto it = objects_.find(id);
    return it != objects_.end() ? &it->second : nullptr;
}

IRecomputable* ObjectRegistry::findRecomputable(ObjectId id) const noexcept {
    const ObjectRef* ref = find(id);
    if (ref == nullptr) return nullptr;
    if (auto* const* recomputable = std::get_if<IRecomputable*>(ref)) return *recomputable;
    return nullptr;
}

std::size_t ObjectRegistry::size() const noexcept {
    return objects_.size();
}

} // namespace paramcad
