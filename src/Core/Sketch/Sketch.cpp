#include "Core/Sketch/Sketch.h"
#include <algorithm>
#include <utility>

namespace paramcad {

Sketch::Sketch(std::string name)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)) {}

Sketch::Sketch(ObjectId id, std::string name, SketchFrame frame)
    : id_(RestoreObjectId(id)), name_(std::move(name)), frame_(frame) {}

SketchEntityId Sketch::addEntity(SketchGeometry geometry) {
    if (!IsValidSketchGeometry(geometry)) return kInvalidSketchEntityId;
    const SketchEntityId id = NextSketchEntityId();
    entities_.push_back(SketchEntity{id, std::move(geometry)});
    return id;
}

SketchEntityId Sketch::addPoint(Vec2 position) {
    return addEntity(SketchPoint{position});
}

SketchEntityId Sketch::addLine(Vec2 start, Vec2 end) {
    return addEntity(SketchLine{start, end});
}

SketchEntityId Sketch::addCircle(Vec2 center, double radiusMm) {
    return addEntity(SketchCircle{center, radiusMm});
}

SketchEntityId Sketch::addArc(Vec2 center, double radiusMm, double startAngleRad,
                              double endAngleRad, bool counterClockwise) {
    return addEntity(
        SketchArc{center, radiusMm, startAngleRad, endAngleRad, counterClockwise});
}

bool Sketch::restoreEntity(SketchEntityId id, SketchGeometry geometry) {
    if (id == kInvalidSketchEntityId) return false;
    if (findEntity(id) != nullptr) return false; // duplicate id within this sketch
    entities_.push_back(SketchEntity{RestoreSketchEntityId(id), std::move(geometry)});
    return true;
}

bool Sketch::removeEntity(SketchEntityId id) {
    const auto it = std::find_if(entities_.begin(), entities_.end(),
                                 [id](const SketchEntity& e) { return e.id == id; });
    if (it == entities_.end()) return false;
    entities_.erase(it);
    return true;
}

const SketchEntity* Sketch::findEntity(SketchEntityId id) const noexcept {
    const auto it = std::find_if(entities_.begin(), entities_.end(),
                                 [id](const SketchEntity& e) { return e.id == id; });
    return it != entities_.end() ? &*it : nullptr;
}

} // namespace paramcad
