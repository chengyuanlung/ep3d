#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Feature/Feature.h"
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

    // Destroys the owned feature with this id; false if this body owns no such
    // feature. Called by PartDocument::removeObject, which is responsible for
    // detaching the feature from the graph and registry FIRST -- a feature
    // still reachable from either of those must never be destroyed here.
    bool removeFeature(ObjectId id);

    ObjectId id_;
    std::string name_;
    std::vector<std::unique_ptr<Feature>> features_;
};

} // namespace paramcad
