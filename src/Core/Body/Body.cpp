#include "Core/Body/Body.h"
#include <algorithm>
#include <utility>

namespace paramcad {

Body::Body(std::string name)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)) {}

Body::Body(ObjectId id, std::string name)
    : id_(RestoreObjectId(id)), name_(std::move(name)) {}

bool Body::removeFeature(const Feature* feature) {
    if (feature == nullptr) return false;
    for (auto it = features_.begin(); it != features_.end(); ++it) {
        if (it->get() != feature) continue;
        features_.erase(it);
        return true;
    }
    return false;
}

bool Body::moveFeatureToIndex(const Feature* feature, std::size_t index) {
    if (feature == nullptr) return false;
    std::size_t from = features_.size();
    for (std::size_t i = 0; i < features_.size(); ++i)
        if (features_[i].get() == feature) from = i;
    if (from == features_.size()) return false;
    if (index >= features_.size()) index = features_.size() - 1;
    if (from == index) return true;
    // std::rotate over the affected span: everything between the two positions
    // shifts by one and nothing else moves, which is what "put it back where it
    // was" means when the rest of the list is unchanged.
    if (from < index)
        std::rotate(features_.begin() + static_cast<std::ptrdiff_t>(from),
                    features_.begin() + static_cast<std::ptrdiff_t>(from) + 1,
                    features_.begin() + static_cast<std::ptrdiff_t>(index) + 1);
    else
        std::rotate(features_.begin() + static_cast<std::ptrdiff_t>(index),
                    features_.begin() + static_cast<std::ptrdiff_t>(from),
                    features_.begin() + static_cast<std::ptrdiff_t>(from) + 1);
    return true;
}

} // namespace paramcad
