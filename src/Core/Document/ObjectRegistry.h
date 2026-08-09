#pragma once

#include "Core/Document/ObjectId.h"
#include <cstddef>
#include <unordered_map>
#include <variant>

namespace paramcad {

class Parameter;
class Body;
class Feature;
class Material;
class Sketch;
class IRecomputable;

// Document-local ObjectId -> object lookup (ADR-010). Stores type-safe
// NON-OWNING tagged handles; ownership is unchanged (PartDocument owns
// Parameters/Bodies, Body owns Features, IRecomputable externals such as test
// stubs are owned by their creator). A registered pointer is valid exactly
// while the owner holds the object; every public mutation path that destroys
// an object goes through PartDocument::removeObject, which unhooks graph and
// registry BEFORE the owner erases. Handles are runtime-only and are never
// serialized. Average O(1) lookup; never a scan of document containers.
class ObjectRegistry {
public:
    // Sketch* joins in M4: a Sketch is a document object that participates in
    // the dependency graph as a dirty source, exactly like Parameter and
    // Material (ADR-M4-005).
    using ObjectRef =
        std::variant<Parameter*, Body*, Feature*, Material*, Sketch*, IRecomputable*>;

    // Rejects kInvalidObjectId, duplicate ids, null handles, and handles
    // whose ->id() differs from the registered id (returns false).
    bool registerObject(ObjectId id, ObjectRef ref);
    bool unregisterObject(ObjectId id) noexcept; // false if unknown
    bool contains(ObjectId id) const noexcept;
    const ObjectRef* find(ObjectId id) const noexcept; // nullptr if unknown
    IRecomputable* findRecomputable(ObjectId id) const noexcept;
    std::size_t size() const noexcept;

private:
    std::unordered_map<ObjectId, ObjectRef> objects_;
};

} // namespace paramcad
