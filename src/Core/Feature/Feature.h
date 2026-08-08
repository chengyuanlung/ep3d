#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Feature/ComputeState.h"
#include <string>
#include <string_view>

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

    // Discriminator used by the serializer to persist/restore the correct
    // concrete feature type (ADR-M3-005) -- replaces the earlier
    // dynamic_cast-based dispatch (ADR-009/012 follow-up, closed in M3).
    virtual std::string_view typeName() const noexcept = 0;

    // Vestigial M1 recompute contract (no RecomputeContext, bool-only). NOT
    // called by the M2/M3 document recompute engine -- IRecomputable::
    // recompute(const RecomputeContext&) is the real execution path once a
    // Feature also implements IRecomputable (ADR-M3-004). Kept unchanged to
    // avoid touching existing M0-M2 behavior; documented accepted debt for a
    // possible M4 cleanup (Feature : public IRecomputable).
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
