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

private:
    ObjectId id_;
    std::string name_;
    std::vector<std::unique_ptr<Feature>> features_;
};

} // namespace paramcad
