#include "Core/Feature/Feature.h"
#include <utility>

namespace paramcad {

Feature::Feature(std::string name)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)) {}

void Feature::setSuppressed(bool suppressed) noexcept {
    state_ = suppressed ? ComputeState::Suppressed : ComputeState::Dirty;
}

} // namespace paramcad
