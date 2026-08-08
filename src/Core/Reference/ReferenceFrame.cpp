#include "Core/Reference/ReferenceFrame.h"
#include <utility>

namespace paramcad {

ReferenceFrame::ReferenceFrame(std::string name, ObjectId parentFrameId)
    : id_(ObjectIdGenerator::Next()), parentFrameId_(parentFrameId), name_(std::move(name)) {}

} // namespace paramcad
