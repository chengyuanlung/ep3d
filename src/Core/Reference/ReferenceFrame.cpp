#include "Core/Reference/ReferenceFrame.h"
#include <utility>

namespace paramcad {

ReferenceFrame::ReferenceFrame(std::string name, ObjectId parentFrameId)
    : id_(ObjectIdGenerator::Next()), parentFrameId_(parentFrameId), name_(std::move(name)) {}

ReferenceFrame::ReferenceFrame(ObjectId id, std::string name, ObjectId parentFrameId,
                               const Transform3D& localTransform)
    : id_(RestoreObjectId(id)), parentFrameId_(parentFrameId), name_(std::move(name)),
      localTransform_(localTransform) {}

} // namespace paramcad
