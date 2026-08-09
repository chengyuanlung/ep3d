#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Sketch/SketchFrame.h"
#include "Core/Sketch/SketchTypes.h"
#include <string>
#include <vector>

namespace paramcad {

// A 2D sketch: a support plane plus a set of stably-identified entities in
// sketch-local (u,v) millimetres (ADR-M4-001/002).
//
// A Sketch is a document object with an ObjectId, registered in ObjectRegistry
// and participating in the dependency graph as a DIRTY SOURCE, exactly like
// Parameter and Material (ADR-011's pattern, reused unchanged) -- editing it
// dirties dependent PadFeatures. It is not an IRecomputable: it has no derived
// state of its own to recompute. Profile validation is a pure function run
// inside PadFeature::recompute and is deliberately not cached here
// (ADR-M4-005).
//
// Entities are sub-objects, NOT registered in ObjectRegistry: nothing outside
// their Sketch references an individual entity in M4.
class Sketch {
public:
    explicit Sketch(std::string name);
    // Restore constructor (deserialization): keeps the persisted id and frame.
    Sketch(ObjectId id, std::string name, SketchFrame frame);

    ObjectId id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }

    const SketchFrame& frame() const noexcept { return frame_; }
    void setFrame(const SketchFrame& frame) noexcept { frame_ = frame; }

    // --- Entity creation ---------------------------------------------------
    // Each returns the new entity's stable id, or kInvalidSketchEntityId if the
    // geometry is invalid (IsValidSketchGeometry) -- invalid geometry is never
    // stored, so it cannot reach a profile through this path. The profile
    // validator re-checks anyway, because a restored entity can carry bad data
    // from a hand-edited document that never went through these methods.
    SketchEntityId addPoint(Vec2 position);
    SketchEntityId addLine(Vec2 start, Vec2 end);
    SketchEntityId addCircle(Vec2 center, double radiusMm);
    SketchEntityId addArc(Vec2 center, double radiusMm, double startAngleRad,
                          double endAngleRad, bool counterClockwise = true);

    // Restore path (deserialization): keeps the persisted entity id and
    // advances the generator past it. Rejects a duplicate id within this
    // sketch, and -- unlike add* -- accepts geometry the validator would
    // reject, so a document round-trips losslessly and the invalid entity
    // surfaces as a profile diagnostic rather than silently disappearing.
    bool restoreEntity(SketchEntityId id, SketchGeometry geometry);

    // False if this sketch owns no entity with that id.
    bool removeEntity(SketchEntityId id);

    // Lookup is always by id, never by position (ADR-M4-001): removal does not
    // renumber anything, and no caller may treat an index as identity.
    const SketchEntity* findEntity(SketchEntityId id) const noexcept;

    const std::vector<SketchEntity>& entities() const noexcept { return entities_; }

private:
    SketchEntityId addEntity(SketchGeometry geometry);

    ObjectId id_;
    std::string name_;
    SketchFrame frame_{};
    std::vector<SketchEntity> entities_;
};

} // namespace paramcad
