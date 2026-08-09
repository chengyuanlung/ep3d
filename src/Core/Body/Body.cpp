#include "Core/Body/Body.h"
#include <utility>

namespace paramcad {

Body::Body(std::string name)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)) {}

Body::Body(ObjectId id, std::string name)
    : id_(RestoreObjectId(id)), name_(std::move(name)) {}

bool Body::removeFeature(ObjectId id) {
    for (auto it = features_.begin(); it != features_.end(); ++it) {
        if ((*it)->id() != id) continue;
        features_.erase(it);
        return true;
    }
    return false;
}

} // namespace paramcad
