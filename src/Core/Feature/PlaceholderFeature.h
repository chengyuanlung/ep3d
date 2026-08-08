#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Feature/Feature.h"
#include <string>

namespace paramcad {

// Generic stand-in for features whose concrete behavior is not yet modeled
// (and for all features restored from serialized documents in schema v1).
// Carries the persisted "type" string so it survives round-trips.
class PlaceholderFeature final : public Feature {
public:
    PlaceholderFeature(std::string name, std::string typeName);
    // Restore constructor (deserialization): keeps the persisted id and state.
    PlaceholderFeature(ObjectId id, std::string name, ComputeState state,
                       std::string typeName);

    const std::string& typeName() const noexcept { return typeName_; }

    bool recompute() override;

private:
    std::string typeName_;
};

} // namespace paramcad
