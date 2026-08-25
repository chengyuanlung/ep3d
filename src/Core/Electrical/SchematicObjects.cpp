#include "Core/Electrical/SchematicObjects.h"

#include <cmath>
#include <utility>

namespace paramcad {

SymbolPlacement::SymbolPlacement(std::string tag, std::string symbolName, Vec2 positionMm,
                                 ObjectId layerId)
    : id_(ObjectIdGenerator::Next()),
      tag_(std::move(tag)),
      symbolName_(std::move(symbolName)),
      positionMm_(positionMm),
      layerId_(layerId) {}

SymbolPlacement::SymbolPlacement(ObjectId id, std::string tag, std::string symbolName,
                                 Vec2 positionMm, double rotationRad, bool mirrored,
                                 ObjectId layerId)
    : id_(RestoreObjectId(id)),
      tag_(std::move(tag)),
      symbolName_(std::move(symbolName)),
      positionMm_(positionMm),
      rotationRad_(rotationRad),
      mirrored_(mirrored),
      layerId_(layerId) {}

PlacedSymbol SymbolPlacement::asPlaced() const {
    PlacedSymbol placed;
    placed.id = id_;
    placed.tag = tag_;
    placed.symbolName = symbolName_;
    placed.positionMm = positionMm_;
    placed.rotationRad = rotationRad_;
    placed.mirrored = mirrored_;
    return placed;
}

WireEntity::WireEntity(std::vector<Vec2> pointsMm, ObjectId layerId)
    : id_(ObjectIdGenerator::Next()), pointsMm_(std::move(pointsMm)), layerId_(layerId) {}

WireEntity::WireEntity(ObjectId id, std::vector<Vec2> pointsMm, ObjectId layerId,
                       std::string label)
    : id_(RestoreObjectId(id)),
      pointsMm_(std::move(pointsMm)),
      label_(std::move(label)),
      layerId_(layerId) {}

WireRun WireEntity::asRun() const {
    WireRun run;
    run.id = id_;
    run.pointsMm = pointsMm_;
    return run;
}

double WireEntity::lengthMm() const noexcept {
    double total = 0.0;
    for (std::size_t i = 0; i + 1 < pointsMm_.size(); ++i)
        total += std::hypot(pointsMm_[i + 1].x - pointsMm_[i].x,
                            pointsMm_[i + 1].y - pointsMm_[i].y);
    return total;
}

} // namespace paramcad
