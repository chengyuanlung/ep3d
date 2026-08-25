#include "Core/Drawing/SheetPage.h"

namespace paramcad {

SheetPage::SheetPage(std::string name, Sheet paper)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)), paper_(std::move(paper)) {}

SheetPage::SheetPage(ObjectId id, std::string name, Sheet paper)
    : id_(RestoreObjectId(id)), name_(std::move(name)), paper_(std::move(paper)) {}

} // namespace paramcad
