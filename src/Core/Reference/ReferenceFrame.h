#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Geometry/MathTypes.h"
#include <string>

namespace paramcad {

class ReferenceFrame {
public:
    explicit ReferenceFrame(std::string name, ObjectId parentFrameId = kInvalidObjectId);

    ObjectId id() const noexcept { return id_; }
    ObjectId parentFrameId() const noexcept { return parentFrameId_; }
    const std::string& name() const noexcept { return name_; }
    const Transform3D& localTransform() const noexcept { return localTransform_; }

    void setLocalTransform(const Transform3D& transform) { localTransform_ = transform; }

private:
    ObjectId id_;
    ObjectId parentFrameId_;
    std::string name_;
    Transform3D localTransform_{};
};

} // namespace paramcad
