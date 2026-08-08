#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Feature/ComputeState.h"
#include <string>

namespace paramcad {

class Feature {
public:
    explicit Feature(std::string name);
    virtual ~Feature() = default;

    ObjectId id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    ComputeState state() const noexcept { return state_; }

    void markDirty() noexcept { if (state_ != ComputeState::Suppressed) state_ = ComputeState::Dirty; }
    void setSuppressed(bool suppressed) noexcept;

    virtual bool recompute() = 0;

protected:
    // Restore constructor (deserialization): keeps the persisted id and state
    // and advances the id generator past the id so future ids cannot collide.
    Feature(ObjectId id, std::string name, ComputeState state);

    void setState(ComputeState state) noexcept { state_ = state; }

private:
    ObjectId id_;
    std::string name_;
    ComputeState state_{ComputeState::Dirty};
};

} // namespace paramcad
