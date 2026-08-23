#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Feature/ComputeState.h"
#include <string>
#include <utility>
#include <string_view>

namespace paramcad {

class Feature {
public:
    explicit Feature(std::string name);
    virtual ~Feature() = default;

    ObjectId id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    ComputeState state() const noexcept { return state_; }


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
    // PRIVATE, with PartDocument as the only caller (round 4, R1R4-M1).
    //
    // `bodies()` hands out `const unique_ptr<Body>&` and `Body::features()`
    // hands out `const unique_ptr<Feature>&`: constness stops at the pointer
    // BOTH times, so every public mutator on a Feature was reachable through a
    // `const PartDocument&`. Round 2 made `Body::addFeature`/`removeFeature`
    // private, which closed CREATING a feature behind the facade but not
    // MUTATING the ones already there -- R1 changed a feature's ComputeState
    // through a const document and made another document unsaveable, both in
    // code that compiles. This is the same closure `Parameter`'s mutators got
    // in round 3, applied to the door round 3's enumeration missed.
    //
    // `setState` stays PROTECTED: a feature setting its own state inside its
    // own recompute() is the design, and it is not reachable from outside.
    friend class PartDocument;

    void markDirty() noexcept { if (state_ != ComputeState::Suppressed) state_ = ComputeState::Dirty; }
    void setSuppressed(bool suppressed) noexcept;

    ObjectId id_;
    // PRIVATE with PartDocument as the only caller (M17.16, ADR-M17-039).
    //
    // A rename is ONE undo step and must refuse a duplicate; both decisions
    // live in PartDocument::renameObject, and a public setter here would be a
    // way around both. Every other name-writing rule in this file is enforced
    // the same way rather than described in a comment.
    friend class PartDocument;
    void setName(std::string name) { name_ = std::move(name); }

    std::string name_;
    ComputeState state_{ComputeState::Dirty};
};

} // namespace paramcad
