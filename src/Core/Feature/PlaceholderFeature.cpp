#include "Core/Feature/PlaceholderFeature.h"
#include <utility>

namespace paramcad {

PlaceholderFeature::PlaceholderFeature(std::string name, std::string typeName)
    : Feature(std::move(name)), typeName_(std::move(typeName)) {}

PlaceholderFeature::PlaceholderFeature(ObjectId id, std::string name, ComputeState state,
                                       std::string typeName)
    : Feature(id, std::move(name), state), typeName_(std::move(typeName)) {}

bool PlaceholderFeature::recompute() {
    setState(ComputeState::Valid);
    return true;
}

} // namespace paramcad
