#include "Core/Document/ResolveObject.h"

#include "Core/Document/ObjectRegistry.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Recompute/IRecomputable.h"
#include "Core/Sketch/Sketch.h"

#include <optional>
#include <variant>

namespace paramcad {

namespace {

// The const overload yields const pointees (R2R4-M1); these resolvers always
// returned const pointers, so the projection matches their intent.
template <typename T>
const T* ResolveAs(const ObjectRegistry& registry, ObjectId id) {
    // THE GUARD ONE COPY IN TEN HAD, and the mutation gate settled what it is
    // worth: deleting it changes no answer, because the registry holds nothing
    // under the invalid id and says so. It is kept because asking a registry
    // about the id that means "nothing" is not a question with a useful answer,
    // and because the copy that grew it was not wrong to -- but it is
    // defensive, not load-bearing, and now that is written down instead of
    // being nine files' worth of disagreement.
    if (id == kInvalidObjectId) return nullptr;
    const std::optional<ObjectRegistry::ConstObjectRef> ref = registry.find(id);
    if (!ref) return nullptr;
    auto* const* found = std::get_if<const T*>(&*ref);
    return found != nullptr ? *found : nullptr;
}

} // namespace

const Parameter* ResolveParameter(const ObjectRegistry& registry, ObjectId id) {
    return ResolveAs<Parameter>(registry, id);
}

const Sketch* ResolveSketch(const ObjectRegistry& registry, ObjectId id) {
    return ResolveAs<Sketch>(registry, id);
}

const ISolidFeature* ResolveSolidFeature(const ObjectRegistry& registry, ObjectId id) {
    // TWO STEPS, because the registry knows features as IRecomputable and the
    // caller wants the solid interface. The cast is the step every one of the
    // eight copies had to get right, and did.
    const IRecomputable* node = ResolveAs<IRecomputable>(registry, id);
    return node != nullptr ? dynamic_cast<const ISolidFeature*>(node) : nullptr;
}

} // namespace paramcad
