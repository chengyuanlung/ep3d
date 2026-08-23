#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Feature/Feature.h"
#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include <utility>

namespace paramcad {

class Body {
public:
    explicit Body(std::string name);
    // Restore constructor (deserialization): keeps the persisted id and
    // advances the id generator past it so future ids cannot collide.
    Body(ObjectId id, std::string name);

    ObjectId id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    const std::vector<std::unique_ptr<Feature>>& features() const noexcept { return features_; }

    // --- Rollback position (M9.4) -------------------------------------------
    //
    // Features at index >= this are NOT evaluated and NOT displayed. They are
    // not removed and not modified: rollback is a POSITION, not an edit, so a
    // save at any position round-trips the whole history (ADR-M9-004).
    //
    // `kNoRollback` means "evaluate everything" and is the default. It is
    // stored as a count rather than as a feature id on purpose: a position
    // BEFORE the first feature and a position AFTER the last one both have to
    // be expressible, and neither names a feature.
    static constexpr std::size_t kNoRollback = static_cast<std::size_t>(-1);
    // The effective cut: kNoRollback and any out-of-range value clamp to
    // features().size(), so a position can never silently hide a feature that
    // a later edit appended.
    std::size_t rollbackCut() const noexcept {
        return rollback_ > features_.size() ? features_.size() : rollback_;
    }
    bool isRolledBack(std::size_t index) const noexcept { return index >= rollbackCut(); }

private:
    // BOTH mutators are private with PartDocument as the only caller (M8
    // round 2, R2R2-M1): `bodies()` returns const unique_ptrs whose constness
    // stops at the pointer, so a public addFeature let one line build a
    // feature BEHIND every facade door -- no requireConsumableBase, no
    // registry entry, no graph node -- leaving a document that could neither
    // be saved nor repaired (removeObject cannot see an unregistered id).
    // The same accessor hazard was found and fixed for sketches() in M5; this
    // is its Body twin. Every creation path now goes through the facade.
    //
    // The friendship is WIDER than its use -- it also grants access to
    // features_ itself -- and that width is a recorded decision, not an
    // accident (round 3, R1 minor): PartDocument touches only addFeature/
    // removeFeature (verified by review grep), and a passkey idiom to narrow
    // it further was judged churn without behavioral gain. Revisit if
    // PartDocument ever starts reaching into Body's fields.
    friend class PartDocument;

    template <typename T, typename... Args>
    T& addFeature(Args&&... args) {
        auto item = std::make_unique<T>(std::forward<Args>(args)...);
        auto& ref = *item;
        features_.push_back(std::move(item));
        return ref;
    }

    // Destroys the owned feature BY POINTER IDENTITY; false if this body does
    // not own that object. Called by PartDocument::removeObject, which is
    // responsible for detaching the feature from the graph and registry FIRST
    // -- a feature still reachable from either of those must never be
    // destroyed here.
    //
    // By identity and not by id (round 4, R1R4-C1): the id form erased the
    // FIRST feature carrying the id, so when a duplicate-id state existed --
    // R1R4-C1 built one through the public facade -- removeObject unregistered
    // one feature and destroyed a DIFFERENT one, leaving the survivor
    // unregistered, graph-less and unremovable. The restore guards now make
    // that state unconstructible; erasing the object the caller actually
    // resolved means this function cannot pick the wrong one even if some
    // future path recreates it.
    bool removeFeature(const Feature* feature);

    // Moves an owned feature to `index`, preserving the relative order of
    // everything else. Used by undo (M9.1) to put a restored feature back where
    // it was: the restore facade appends, and feature ORDER is load-bearing --
    // the loader requires a consumer to follow its base in the array, and
    // `validateSaveable` enforces the same rule, so a middle feature restored
    // at the end would produce a document that cannot be saved.
    //
    // By POINTER, like removeFeature and for the same reason. False if this
    // body does not own that object; an index past the end clamps to the end.
    bool moveFeatureToIndex(const Feature* feature, std::size_t index);

    // Set only through PartDocument::setRollbackPosition, so the mass source
    // and the dirty set are updated in the same step.
    void setRollback(std::size_t cut) noexcept { rollback_ = cut; }

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
    std::vector<std::unique_ptr<Feature>> features_;
    std::size_t rollback_ = kNoRollback;
};

} // namespace paramcad
