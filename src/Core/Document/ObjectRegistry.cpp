#include "Core/Document/ObjectRegistry.h"
#include "Core/Body/Body.h"
#include "Core/Feature/Feature.h"
#include "Core/Material/Material.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Recompute/IRecomputable.h"
#include "Core/Reference/ReferenceFrame.h"
#include "Core/Connector/Connector.h"
#include "Core/Sketch/Sketch.h"
#include <type_traits>

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

ObjectRegistry::ObjectRef* ObjectRegistry::find(ObjectId id) noexcept {
    const auto it = objects_.find(id);
    return it != objects_.end() ? &it->second : nullptr;
}

std::optional<ObjectRegistry::ConstObjectRef> ObjectRegistry::find(ObjectId id) const noexcept {
    const auto it = objects_.find(id);
    if (it == objects_.end()) return std::nullopt;
    // Project each alternative onto its const twin. The projection is the whole
    // point: an inspector must not be able to reach a mutator through a handle
    // it obtained from a const document (R2R4-M1).
    return std::visit([](auto* object) -> ConstObjectRef { return object; }, it->second);
}

const IRecomputable* ObjectRegistry::findRecomputable(ObjectId id) const noexcept {
    return const_cast<ObjectRegistry*>(this)->findRecomputable(id);
}

IRecomputable* ObjectRegistry::findRecomputable(ObjectId id) noexcept {
    const ObjectRef* ref = find(id);
    if (ref == nullptr) return nullptr;
    // Whether an object is recomputable is a property of its TYPE, not of which
    // handle alternative the caller happened to register it under. Matching only
    // the IRecomputable* alternative made that distinction by accident: a Sketch
    // registered as Sketch* (so PadFeature can resolve its profile) would have
    // reported "not recomputable" and silently never solved, while registering
    // it as IRecomputable* instead would have made every Pad lose its sketch.
    // Upcasting from the concrete alternative removes the choice entirely.
    return std::visit(
        [](auto* object) -> IRecomputable* {
            using T = std::remove_pointer_t<decltype(object)>;
            if constexpr (std::is_base_of_v<IRecomputable, T>) {
                return object; // implicit derived-to-base upcast
            } else {
                return nullptr;
            }
        },
        *ref);
}

std::size_t ObjectRegistry::size() const noexcept {
    return objects_.size();
}

} // namespace paramcad
