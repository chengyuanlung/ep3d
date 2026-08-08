#include "Core/Feature/Feature.h"
#include <utility>

namespace paramcad {

Feature::Feature(std::string name)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)) {}

Feature::Feature(ObjectId id, std::string name, ComputeState state)
    : id_(RestoreObjectId(id)), name_(std::move(name)), state_(state) {}

void Feature::setSuppressed(bool suppressed) noexcept {
    state_ = suppressed ? ComputeState::Suppressed : ComputeState::Dirty;
}

} // namespace paramcad
