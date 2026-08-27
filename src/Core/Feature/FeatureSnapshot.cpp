#include "Core/Feature/FeatureSnapshot.h"

#include "Core/Body/Body.h"
#include "Core/Document/PartDocument.h"
#include "Core/Feature/BoxFeature.h"
#include "Core/Feature/EdgeDressFeatures.h"
#include "Core/Feature/Feature.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/PlaceholderFeature.h"
#include "Core/Feature/PocketFeature.h"
#include "Core/Feature/SheetContourFeature.h"
#include "Core/Feature/BooleanFeature.h"
#include "Core/Feature/DraftFeature.h"
#include "Core/Feature/HoleFeature.h"
#include "Core/Feature/ImportFeature.h"
#include "Core/Feature/LoftFeature.h"
#include "Core/Feature/ShellFeature.h"
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
    } else if (const auto* imported = dynamic_cast<const ImportFeature*>(&feature)) {
        snapshot.importPath = imported->path();
        snapshot.materialId = imported->materialId();
    } else if (const auto* boolean = dynamic_cast<const BooleanFeature*>(&feature)) {
        snapshot.baseFeatureId = boolean->targetFeatureId();
        snapshot.toolFeatureId = boolean->toolFeatureId();
        snapshot.booleanOperation = boolean->operation();
        snapshot.materialId = boolean->materialId();
    } else if (const auto* circular = dynamic_cast<const CircularPatternFeature*>(&feature)) {
        // BEFORE PatternFeature and before TransformFeature, for the reason
        // EdgeDressFeature is tested before its twins: a base class swallows
        // the derived one and silently drops its fields.
        snapshot.baseFeatureId = circular->baseFeatureId();
        snapshot.frameId = circular->frameId();
        snapshot.countParameterId = circular->countParameterId();
        snapshot.spacingParameterId = circular->stepParameterId();
        snapshot.materialId = circular->materialId();
    } else if (const auto* curve = dynamic_cast<const CurvePatternFeature*>(&feature)) {
        snapshot.baseFeatureId = curve->baseFeatureId();
        snapshot.sketchId = curve->pathSketchId();
        snapshot.countParameterId = curve->countParameterId();
        snapshot.materialId = curve->materialId();
    } else if (const auto* shell = dynamic_cast<const ShellFeature*>(&feature)) {
        snapshot.baseFeatureId = shell->baseFeatureId();
        snapshot.faceSelection = shell->openFaces();
        snapshot.sizeParameterId = shell->thicknessParameterId();
        snapshot.materialId = shell->materialId();
    } else if (const auto* draft = dynamic_cast<const DraftFeature*>(&feature)) {
        snapshot.baseFeatureId = draft->baseFeatureId();
        snapshot.faceSelection = draft->faces();
        snapshot.neutralFace = draft->neutralFace();
        snapshot.sizeParameterId = draft->angleParameterId();
        snapshot.materialId = draft->materialId();
    } else if (const auto* hole = dynamic_cast<const HoleFeature*>(&feature)) {
        snapshot.baseFeatureId = hole->baseFeatureId();
        snapshot.sketchId = hole->sketchId();
        snapshot.diameterParameterId = hole->diameterParameterId();
        snapshot.holeDepthParameterId = hole->depthParameterId();
        snapshot.holeKind = hole->kind();
        snapshot.holeScrew = hole->screw();
        snapshot.materialId = hole->materialId();
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
    } else if (const auto* folded = dynamic_cast<const SheetContourFeature*>(&feature)) {
        // M52's feature was added without this, and nothing said so: the part
        // saved cleanly, opened cleanly, and came back with no fold in it.
        // M53's first end-to-end test is what noticed, because it asked the
        // drawing to unfold a part that had been through a file.
        snapshot.sheetContour = folded->contour();
        snapshot.widthParameterId = folded->widthParameterId();
        snapshot.materialId = folded->materialId();
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
    if (type == "SheetContour")
        return document.restoreSheetContourFeature(body, snapshot.id, snapshot.name,
                                                   snapshot.state, snapshot.sheetContour,
                                                   snapshot.widthParameterId,
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
    if (type == "Import")
        return document.restoreImportFeature(body, snapshot.id, snapshot.name, snapshot.state,
                                             snapshot.importPath, snapshot.materialId);
    if (type == "Boolean")
        return document.restoreBooleanFeature(body, snapshot.id, snapshot.name, snapshot.state,
                                              snapshot.booleanOperation, snapshot.baseFeatureId,
                                              snapshot.toolFeatureId, snapshot.materialId);
    if (type == "CircularPattern")
        return document.restoreCircularPatternFeature(
            body, snapshot.id, snapshot.name, snapshot.state, snapshot.baseFeatureId,
            snapshot.frameId, snapshot.countParameterId, snapshot.spacingParameterId,
            snapshot.materialId);
    if (type == "CurvePattern")
        return document.restoreCurvePatternFeature(body, snapshot.id, snapshot.name,
                                                   snapshot.state, snapshot.baseFeatureId,
                                                   snapshot.sketchId, snapshot.countParameterId,
                                                   snapshot.materialId);
    if (type == "Shell")
        return document.restoreShellFeature(body, snapshot.id, snapshot.name, snapshot.state,
                                            snapshot.baseFeatureId, snapshot.faceSelection,
                                            snapshot.sizeParameterId, snapshot.materialId);
    if (type == "Draft")
        return document.restoreDraftFeature(body, snapshot.id, snapshot.name, snapshot.state,
                                            snapshot.baseFeatureId, snapshot.faceSelection,
                                            snapshot.neutralFace, snapshot.sizeParameterId,
                                            snapshot.materialId);
    if (type == "Hole") {
        HoleFeature& hole = document.restoreHoleFeature(
            body, snapshot.id, snapshot.name, snapshot.state, snapshot.baseFeatureId,
            snapshot.sketchId, snapshot.diameterParameterId, snapshot.holeDepthParameterId,
            snapshot.materialId);
        hole.setKind(snapshot.holeKind);
        hole.setScrew(snapshot.holeScrew);
        return hole;
    }
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
