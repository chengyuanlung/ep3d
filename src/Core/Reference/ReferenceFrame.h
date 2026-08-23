#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Geometry/MathTypes.h"
#include <string>

namespace paramcad {

class PartDocument;

// A named local coordinate system with a parent (M10, ADR-M10-001).
//
// FIRST-CLASS SINCE M10: registered, graph-participating, persisted, undoable.
// Before M10 this type existed with none of that -- `addFrame` pushed it into a
// vector and stopped, so a frame had an ObjectId nothing could resolve,
// `removeObject` could not see it, and a frame the user moved was lost on save
// (ADR-009 D6 recorded that deliberately; ADR-M10-001 supersedes it).
//
// The WORLD transform is not here, and that is the rule rather than an
// omission: it is composed from the parent chain on demand
// (`PartDocument::worldTransform`), never stored (ADR-M10-002). Storing both a
// local and a world transform is how two truths disagree the moment a parent
// moves and something forgets to update.
class ReferenceFrame {
public:
    explicit ReferenceFrame(std::string name, ObjectId parentFrameId = kInvalidObjectId);
    // Restore path (deserialization): keeps the persisted id and advances the
    // generator past it, exactly as every other restored type does.
    ReferenceFrame(ObjectId id, std::string name, ObjectId parentFrameId,
                   const Transform3D& localTransform);

    ObjectId id() const noexcept { return id_; }
    ObjectId parentFrameId() const noexcept { return parentFrameId_; }
    const std::string& name() const noexcept { return name_; }
    const Transform3D& localTransform() const noexcept { return localTransform_; }

private:
    // PRIVATE with PartDocument as the only caller (M10, closing the door
    // review round 3 recorded as "inert today -- and closed the
    // sketches()/bodies()/Parameter way THE DAY A CONSUMER APPEARS"). M10 is
    // that day: a sketch now reads its support frame, so a transform changed
    // behind the facade would leave the graph undirtied and the geometry stale
    // -- the exact class ADR-M3-004 exists to prevent.
    friend class PartDocument;

    void setLocalTransform(const Transform3D& transform) { localTransform_ = transform; }
    void setParentFrameId(ObjectId parentFrameId) noexcept { parentFrameId_ = parentFrameId; }

    ObjectId id_;
    ObjectId parentFrameId_;
    std::string name_;
    Transform3D localTransform_{};
};

} // namespace paramcad
