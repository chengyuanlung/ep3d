#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Feature/Feature.h"
#include <string>
#include <string_view>

namespace paramcad {

// Generic stand-in for features whose concrete behavior is not yet modeled
// (and for all features restored from serialized documents whose "type"
// string does not match a concrete Feature type known to this build).
// Carries the persisted "type" string so it survives round-trips losslessly
// even for a type this build cannot construct.
class PlaceholderFeature final : public Feature {
public:
    PlaceholderFeature(std::string name, std::string typeName);
    // Restore constructor (deserialization): keeps the persisted id and state.
    PlaceholderFeature(ObjectId id, std::string name, ComputeState state,
                       std::string typeName);

    std::string_view typeName() const noexcept override { return typeName_; }

    bool recompute() override;

private:
    std::string typeName_;
};

} // namespace paramcad
