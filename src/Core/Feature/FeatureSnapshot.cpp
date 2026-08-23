#include "Core/Feature/FeatureSnapshot.h"

#include "Core/Body/Body.h"
#include "Core/Document/PartDocument.h"
#include "Core/Feature/BoxFeature.h"
#include "Core/Feature/EdgeDressFeatures.h"
#include "Core/Feature/Feature.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/PlaceholderFeature.h"
#include "Core/Feature/PocketFeature.h"
#include "Core/Feature/LoftFeature.h"
#include "Core/Feature/RevolveFeature.h"
#include "Core/Feature/SweepFeature.h"
#include "Core/Feature/TransformFeatures.h"

#include <string>
#include <utility>

namespace paramcad {

FeatureSnapshot SnapshotFeature(const Feature& feature) {
    FeatureSnapshot snapshot;
    snapshot.id = feature.id();
    snapshot.name = feature.name();
    snapshot.typeName = std::string(feature.typeName());
    snapshot.state = feature.state();

    // Ordered exactly as the serializer's save-side dispatch is, and for the
    // same reason: EdgeDressFeature must be tested before its concrete twins
    // would be, and PocketFeature after the dress branch, so one shared record
    // shape serves Fillet and Chamfer with `typeName` as the discriminator.
    if (const auto* box = dynamic_cast<const BoxFeature*>(&feature)) {
        snapshot.widthParameterId = box->widthParameterId();
        snapshot.heightParameterId = box->heightParameterId();
        snapshot.depthParameterId = box->depthParameterId();
        snapshot.materialId = box->materialId();
    } else if (const auto* pad = dynamic_cast<const PadFeature*>(&feature)) {
        snapshot.sketchId = pad->sketchId();
        snapshot.lengthParameterId = pad->lengthParameterId();
        snapshot.materialId = pad->materialId();
    } else if (const auto* revolve = dynamic_cast<const RevolveFeature*>(&feature)) {
        snapshot.sketchId = revolve->sketchId();
        snapshot.axisEntityId = ToObjectId(revolve->axisEntityId());
        snapshot.angleParameterId = revolve->angleParameterId();
        snapshot.materialId = revolve->materialId();
    } else if (const auto* sweep = dynamic_cast<const SweepFeature*>(&feature)) {
        snapshot.sketchId = sweep->profileSketchId();
        snapshot.pathSketchId = sweep->pathSketchId();
        snapshot.materialId = sweep->materialId();
    } else if (const auto* loft = dynamic_cast<const LoftFeature*>(&feature)) {
        snapshot.sectionSketchIds = loft->sectionSketchIds();
        snapshot.materialId = loft->materialId();
    } else if (const auto* dress = dynamic_cast<const EdgeDressFeature*>(&feature)) {
        snapshot.baseFeatureId = dress->baseFeatureId();
        snapshot.sizeParameterId = dress->sizeParameterId();
        snapshot.materialId = dress->materialId();
        // WHICH EDGES (M17.12). Missing here, undo brought a fillet back
        // dressing every edge instead of the face the user had chosen -- the
        // shape changed and nothing said so, which is the worst way for a
        // field to be forgotten. FeatureSnapshot has two producers, this and
        // the loader, and a field added to one has to reach both.
        snapshot.edgeSelection = dress->edgeSelection();
    } else if (const auto* pattern = dynamic_cast<const PatternFeature*>(&feature)) {
        snapshot.baseFeatureId = pattern->baseFeatureId();
        snapshot.frameId = pattern->frameId();
        snapshot.countParameterId = pattern->countParameterId();
        snapshot.spacingParameterId = pattern->spacingParameterId();
        snapshot.materialId = pattern->materialId();
    } else if (const auto* transform = dynamic_cast<const TransformFeature*>(&feature)) {
        // Mirror, and anything later that is a transform without extra
        // parameters. Tested AFTER Pattern for the same reason EdgeDressFeature
        // is tested before its twins: the base class would swallow the derived
        // one and silently drop its fields.
        snapshot.baseFeatureId = transform->baseFeatureId();
        snapshot.frameId = transform->frameId();
        snapshot.materialId = transform->materialId();
    } else if (const auto* pocket = dynamic_cast<const PocketFeature*>(&feature)) {
        snapshot.baseFeatureId = pocket->baseFeatureId();
        snapshot.sketchId = pocket->sketchId();
        snapshot.depthParameterId = pocket->depthParameterId();
        snapshot.materialId = pocket->materialId();
    }
    return snapshot;
}

Feature& RestoreFeatureFromSnapshot(PartDocument& document, Body& body,
                                    const FeatureSnapshot& snapshot) {
    // Dispatch by the persisted TYPE NAME, never by probing (ADR-M3-005). This
    // is the one dispatch: the serializer's loader calls it, and so does undo.
    const std::string& type = snapshot.typeName;
    if (type == "Box")
        return document.restoreBoxFeature(body, snapshot.id, snapshot.name, snapshot.state,
                                          snapshot.widthParameterId, snapshot.heightParameterId,
                                          snapshot.depthParameterId, snapshot.materialId);
    if (type == "Pad")
        return document.restorePadFeature(body, snapshot.id, snapshot.name, snapshot.state,
                                          snapshot.sketchId, snapshot.lengthParameterId,
                                          snapshot.materialId);
    if (type == "Pocket")
        return document.restorePocketFeature(body, snapshot.id, snapshot.name, snapshot.state,
                                             snapshot.baseFeatureId, snapshot.sketchId,
                                             snapshot.depthParameterId, snapshot.materialId);
    if (type == "Revolve")
        return document.restoreRevolveFeature(
            body, snapshot.id, snapshot.name, snapshot.state, snapshot.sketchId,
            static_cast<SketchEntityId>(snapshot.axisEntityId), snapshot.angleParameterId,
            snapshot.materialId);
    if (type == "Sweep")
        return document.restoreSweepFeature(body, snapshot.id, snapshot.name, snapshot.state,
                                            snapshot.sketchId, snapshot.pathSketchId,
                                            snapshot.materialId);
    if (type == "Loft")
        return document.restoreLoftFeature(body, snapshot.id, snapshot.name, snapshot.state,
                                           snapshot.sectionSketchIds, snapshot.materialId);
    if (type == "Mirror")
        return document.restoreMirrorFeature(body, snapshot.id, snapshot.name, snapshot.state,
                                             snapshot.baseFeatureId, snapshot.frameId,
                                             snapshot.materialId);
    if (type == "Pattern")
        return document.restorePatternFeature(body, snapshot.id, snapshot.name, snapshot.state,
                                              snapshot.baseFeatureId, snapshot.frameId,
                                              snapshot.countParameterId,
                                              snapshot.spacingParameterId, snapshot.materialId);
    // The edge selection is applied AFTER construction rather than threaded
    // through two more restore signatures: it is not identity, nothing else
    // depends on it, and a seventh parameter on each of these would be one more
    // thing for the next feature kind to forget.
    if (type == "Fillet") {
        FilletFeature& fillet =
            document.restoreFilletFeature(body, snapshot.id, snapshot.name, snapshot.state,
                                          snapshot.baseFeatureId, snapshot.sizeParameterId,
                                          snapshot.materialId);
        document.setFeatureEdgeSelection(fillet.id(), snapshot.edgeSelection);
        return fillet;
    }
    if (type == "Chamfer") {
        ChamferFeature& chamfer =
            document.restoreChamferFeature(body, snapshot.id, snapshot.name, snapshot.state,
                                           snapshot.baseFeatureId, snapshot.sizeParameterId,
                                           snapshot.materialId);
        document.setFeatureEdgeSelection(chamfer.id(), snapshot.edgeSelection);
        return chamfer;
    }
    return document.restorePlaceholderFeature(body, snapshot.id, snapshot.name, snapshot.state,
                                              snapshot.typeName);
}

} // namespace paramcad
