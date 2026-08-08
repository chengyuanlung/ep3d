#include "Core/Body/Body.h"
#include <utility>

namespace paramcad {

Body::Body(std::string name)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)) {}

Body::Body(ObjectId id, std::string name)
    : id_(RestoreObjectId(id)), name_(std::move(name)) {}

} // namespace paramcad
