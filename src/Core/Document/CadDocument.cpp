#include "Core/Document/CadDocument.h"
#include <utility>

namespace paramcad {

CadDocument::CadDocument(std::string name)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)) {}

CadDocument::CadDocument(ObjectId id, std::string name)
    : id_(RestoreObjectId(id)), name_(std::move(name)) {}

} // namespace paramcad
