#include "Core/Drawing/DrawingDimension.h"

#include <utility>

namespace paramcad {

std::string_view toString(DimensionAnchorKind kind) noexcept {
    switch (kind) {
        case DimensionAnchorKind::Free: return "Free";
        case DimensionAnchorKind::Entity: return "Entity";
        case DimensionAnchorKind::InView: return "InView";
    }
    return "Free";
}

std::string_view toString(ViewPointRole role) noexcept {
    switch (role) {
        case ViewPointRole::Corner: return "corner";
        case ViewPointRole::Middle: return "middle";
        case ViewPointRole::Centre: return "centre";
        case ViewPointRole::CurveEnd: return "curve-end";
    }
    return "corner";
}

bool ParseViewPointRole(std::string_view text, ViewPointRole& into) noexcept {
    // READ FROM THE SAME LIST IT IS WRITTEN FROM. A role this build does not
    // know must NOT become "corner": a centre read as a corner is exactly the
    // silent re-attachment this whole mechanism exists to stop.
    for (const ViewPointRole role : {ViewPointRole::Corner, ViewPointRole::Middle,
                                     ViewPointRole::Centre, ViewPointRole::CurveEnd})
        if (text == toString(role)) {
            into = role;
            return true;
        }
    return false;
}

std::string_view toString(DimensionKind kind) noexcept {
    switch (kind) {
        case DimensionKind::Linear: return "Linear";
        case DimensionKind::Radius: return "Radius";
        case DimensionKind::Diameter: return "Diameter";
        case DimensionKind::Angular: return "Angular";
    }
    return "Linear";
}

std::string_view toString(LinearDirection direction) noexcept {
    switch (direction) {
        case LinearDirection::Aligned: return "Aligned";
        case LinearDirection::Horizontal: return "Horizontal";
        case LinearDirection::Vertical: return "Vertical";
    }
    return "Aligned";
}

DimensionAnchor DimensionAnchor::free(Vec2 sheetMm) {
    DimensionAnchor anchor;
    anchor.kind = DimensionAnchorKind::Free;
    anchor.at = sheetMm;
    return anchor;
}

DimensionAnchor DimensionAnchor::onEntity(ObjectId entityId, int snapIndex) {
    DimensionAnchor anchor;
    anchor.kind = DimensionAnchorKind::Entity;
    anchor.entityId = entityId;
    anchor.snapIndex = snapIndex;
    return anchor;
}

DimensionAnchor DimensionAnchor::inView(ObjectId viewId, Vec2 modelMm, ViewPointRole role,
                                        double toleranceMm) {
    DimensionAnchor anchor;
    anchor.kind = DimensionAnchorKind::InView;
    anchor.viewId = viewId;
    anchor.at = modelMm;
    anchor.role = role;
    anchor.toleranceMm = toleranceMm > 0.0 ? toleranceMm : 5.0;
    return anchor;
}

DrawingDimension::DrawingDimension(DimensionKind kind, DimensionAnchor first,
                                   DimensionAnchor second, Vec2 linePositionMm,
                                   ObjectId styleId, ObjectId layerId)
    : id_(ObjectIdGenerator::Next()),
      kind_(kind),
      first_(first),
      second_(second),
      linePositionMm_(linePositionMm),
      styleId_(styleId),
      layerId_(layerId) {}

DrawingDimension::DrawingDimension(ObjectId id, DimensionKind kind, DimensionAnchor first,
                                   DimensionAnchor second, Vec2 linePositionMm,
                                   ObjectId styleId, ObjectId layerId)
    : id_(RestoreObjectId(id)),
      kind_(kind),
      first_(first),
      second_(second),
      linePositionMm_(linePositionMm),
      styleId_(styleId),
      layerId_(layerId) {}

} // namespace paramcad
