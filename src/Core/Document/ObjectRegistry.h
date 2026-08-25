#pragma once

#include "Core/Document/ObjectId.h"
#include <cstddef>
#include <unordered_map>
#include <optional>
#include <variant>

namespace paramcad {

class Parameter;
class Body;
class Feature;
class Material;
class Sketch;
class ReferenceFrame;
class Connector;
class Instance;
class Mate;
class NamedPosition;
class ExplodeView;
class Relation;
class Layer;
class Linetype;
class DrawingEntity;
class DrawingDimension;
class BomTable;
class DimensionStyle;
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
    // the dependency graph (ADR-M4-005) -- as a dirty source in M4, and as a
    // recomputable node since M5. It stays registered under its own concrete
    // alternative in both cases; findRecomputable below derives the capability
    // from the type rather than from the alternative.
    // ReferenceFrame and Connector join in M10, when they become resolvable
    // document objects rather than entries in a vector (ADR-M10-001).
    // Mate joins in M24: it is a named, id-carrying, save-and-load document
    // object like every other, so the one-id-once rule and removeObject have
    // to be able to see it. Instance does NOT appear here -- it registers
    // under IRecomputable, because it is one.
    using ObjectRef = std::variant<Parameter*, Body*, Feature*, Material*, Sketch*,
                                   ReferenceFrame*, Connector*, Mate*, NamedPosition*,
                                   ExplodeView*, Relation*, Layer*, Linetype*,
                                   DrawingEntity*, DrawingDimension*, DimensionStyle*,
                                   BomTable*, IRecomputable*>;

    // The same handle with const pointees, for callers that only INSPECT.
    //
    // Round 4, R2R4-M1: `find` was const, but constness stopped at the
    // pointees, so `document.objectRegistry().find(id)` on a `const
    // PartDocument&` handed out mutable `Material*`, `Sketch*` and `Feature*`.
    // That reopened, through one back door, two of the three accessors round 3
    // had just closed -- a reviewer doubled a material's density through a
    // const document and the cached mass stayed `valid`. The header's own
    // "Const-only access; mutation goes through the facade" was false.
    //
    // The fix is the same const-correctness `sketches()` got in M5 and
    // `material()` got in round 3, applied at the registry: the const overload
    // of `find` yields const pointees, the non-const overload (owners only --
    // PartDocument and the recompute engine hold a non-const registry) is
    // unchanged.
    using ConstObjectRef =
        std::variant<const Parameter*, const Body*, const Feature*, const Material*,
                     const Sketch*, const ReferenceFrame*, const Connector*, const Mate*,
                     const NamedPosition*, const ExplodeView*, const Relation*,
                     const Layer*, const Linetype*, const DrawingEntity*,
                     const DrawingDimension*, const DimensionStyle*, const BomTable*,
                     const IRecomputable*>;

    // Rejects kInvalidObjectId, duplicate ids, null handles, and handles
    // whose ->id() differs from the registered id (returns false).
    bool registerObject(ObjectId id, ObjectRef ref);
    bool unregisterObject(ObjectId id) noexcept; // false if unknown
    bool contains(ObjectId id) const noexcept;
    ObjectRef* find(ObjectId id) noexcept;                          // nullptr if unknown
    std::optional<ConstObjectRef> find(ObjectId id) const noexcept; // nullopt if unknown
    IRecomputable* findRecomputable(ObjectId id) noexcept;
    const IRecomputable* findRecomputable(ObjectId id) const noexcept;
    std::size_t size() const noexcept;

private:
    std::unordered_map<ObjectId, ObjectRef> objects_;
};

} // namespace paramcad
