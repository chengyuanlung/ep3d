#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Serialization/DocumentJson.h"
#include "Core/Dependency/DependencyGraph.h"
#include "Core/Feature/BoxFeature.h"
#include "Core/Feature/IMaterialReferencing.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Feature/FeatureSnapshot.h"
#include "Core/Kernel/EdgeQuery.h"
#include "Core/Kernel/FaceQuery.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/PocketFeature.h"
#include "Core/Feature/BooleanFeature.h"
#include "Core/Feature/DraftFeature.h"
#include "Core/Feature/HoleFeature.h"
#include "Core/Feature/ImportFeature.h"
#include "Core/Feature/LoftFeature.h"
#include "Core/Feature/ShellFeature.h"
#include "Core/Feature/RevolveFeature.h"
#include "Core/Feature/SweepFeature.h"
#include "Core/Feature/EdgeDressFeatures.h"
#include "Core/Sketch/Sketch.h"
#include "Core/Feature/PlaceholderFeature.h"
#include "Core/Material/Material.h"
#include "Core/Serialization/JsonValue.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace paramcad {

namespace {

// One geometry value as JSON fields on `entry`.
//
// Extracted when projected reference geometry needed the SAME encoding
// (M17.6). A reference IS a line, a circle, an arc or a point, so it is
// written with the same keys -- and a second copy of this visit would be a
// second format that could drift from the first, in a file the loader reads
// with one parser.
void WriteSketchGeometry(JsonValue& entry, const SketchGeometry& geometry) {
    std::visit(
        [&entry](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, SketchPoint>) {
                entry.set("type", JsonValue::makeString("Point"));
                entry.set("u", JsonValue::makeNumber(value.position.x));
                entry.set("v", JsonValue::makeNumber(value.position.y));
            } else if constexpr (std::is_same_v<T, SketchLine>) {
                entry.set("type", JsonValue::makeString("Line"));
                entry.set("u1", JsonValue::makeNumber(value.start.x));
                entry.set("v1", JsonValue::makeNumber(value.start.y));
                entry.set("u2", JsonValue::makeNumber(value.end.x));
                entry.set("v2", JsonValue::makeNumber(value.end.y));
            } else if constexpr (std::is_same_v<T, SketchCircle>) {
                entry.set("type", JsonValue::makeString("Circle"));
                entry.set("u", JsonValue::makeNumber(value.center.x));
                entry.set("v", JsonValue::makeNumber(value.center.y));
                entry.set("radiusMm", JsonValue::makeNumber(value.radiusMm));
            } else if constexpr (std::is_same_v<T, SketchSpline>) {
                entry.set("type", JsonValue::makeString("Spline"));
                JsonValue points = JsonValue::makeArray();
                for (const Vec2& point : value.points) {
                    JsonValue one = JsonValue::makeObject();
                    one.set("u", JsonValue::makeNumber(point.x));
                    one.set("v", JsonValue::makeNumber(point.y));
                    points.add(std::move(one));
                }
                entry.set("points", std::move(points));
                // ALWAYS, including when false. Closed and open are different
                // curves -- one is a loop on its own and the other has ends a
                // profile chains through -- so a file that omitted it would
                // load as the other shape rather than as a default.
                entry.set("closed", JsonValue::makeBool(value.closed));
                // WRITTEN ONLY WHEN THERE ARE ANY (M18), so every spline
                // written before v24 is byte-identical -- and absent reads as
                // "no handles", which is exactly what those files meant.
                if (!value.handles.empty()) {
                    JsonValue written = JsonValue::makeArray();
                    for (const auto& [index, tangent] : value.handles) {
                        JsonValue one = JsonValue::makeObject();
                        one.set("point", JsonValue::makeNumber(index));
                        // The TANGENT, relative to its point -- not the tip.
                        // Storing the tip would mean a file whose points and
                        // tips disagreed could be written by moving one, and
                        // the reader would have no way to tell which was meant.
                        one.set("du", JsonValue::makeNumber(tangent.x));
                        one.set("dv", JsonValue::makeNumber(tangent.y));
                        written.add(std::move(one));
                    }
                    entry.set("handles", std::move(written));
                }
            } else if constexpr (std::is_same_v<T, SketchEllipse> ||
                                 std::is_same_v<T, SketchEllipticalArc>) {
                entry.set("type",
                          JsonValue::makeString(std::is_same_v<T, SketchEllipse>
                                                    ? "Ellipse"
                                                    : "EllipticalArc"));
                entry.set("u", JsonValue::makeNumber(value.center.x));
                entry.set("v", JsonValue::makeNumber(value.center.y));
                entry.set("majorRadiusMm", JsonValue::makeNumber(value.majorRadiusMm));
                entry.set("minorRadiusMm", JsonValue::makeNumber(value.minorRadiusMm));
                entry.set("rotationRad", JsonValue::makeNumber(value.rotationRad));
                if constexpr (std::is_same_v<T, SketchEllipticalArc>) {
                    // PARAMETERS, and the key says so. Naming them
                    // "startAngleRad" like the circular arc's would invite the
                    // next reader to feed one to atan2, which is right at the
                    // axes and wrong everywhere else.
                    entry.set("startParamRad", JsonValue::makeNumber(value.startParamRad));
                    entry.set("endParamRad", JsonValue::makeNumber(value.endParamRad));
                    entry.set("counterClockwise", JsonValue::makeBool(value.counterClockwise));
                }
            } else {
                static_assert(std::is_same_v<T, SketchArc>);
                entry.set("type", JsonValue::makeString("Arc"));
                entry.set("u", JsonValue::makeNumber(value.center.x));
                entry.set("v", JsonValue::makeNumber(value.center.y));
                entry.set("radiusMm", JsonValue::makeNumber(value.radiusMm));
                entry.set("startAngleRad", JsonValue::makeNumber(value.startAngleRad));
                entry.set("endAngleRad", JsonValue::makeNumber(value.endAngleRad));
                entry.set("counterClockwise", JsonValue::makeBool(value.counterClockwise));
            }
        },
        geometry);
}


// The version counter is shared with every other document type since M23
// (DocumentJson.h): one format, one number.
// v15 added PointLineDistance (M17).
// v14 added construction geometry (M17).
// v13 added Horizontal/VerticalDistance (M17).
// v12 added user-placed dimension positions (M16).
// The shared vocabulary (M23, ADR-M23-003). These used to be spelled out
// here; they moved to DocumentJson when the Assembly serializer needed the
// same header, the same ids, the same transforms and the same frames.
using docjson::connectorOwnerFromString;
using docjson::connectorRoleFromString;
using docjson::FieldError;
using docjson::fieldError;
using docjson::idFromString;
using docjson::idToString;
using docjson::kFormatName;
using docjson::kMinSupportedSchemaVersion;
using docjson::kSchemaVersion;
using docjson::requireField;
using docjson::toString;
using docjson::transformToJson;

// Sub-element names, used in both directions. Written as strings, never as the
// enum's underlying integer: an integer would silently change meaning the day a
// sub-element is inserted into the middle of the enum, and every file already
// on disk would then load as the wrong sub-element without any error
// (ADR-M4-004's "identity is semantic, never positional", applied to enums).
const char* subElementName(SketchSubElement subElement) noexcept {
    switch (subElement) {
        case SketchSubElement::Whole: return "Whole";
        case SketchSubElement::StartPoint: return "StartPoint";
        case SketchSubElement::EndPoint: return "EndPoint";
        case SketchSubElement::CenterPoint: return "CenterPoint";
        case SketchSubElement::SplinePoint: return "SplinePoint";
    }
    return "Whole";
}

std::optional<SketchSubElement> subElementFromString(const std::string& name) {
    if (name == "Whole") return SketchSubElement::Whole;
    if (name == "StartPoint") return SketchSubElement::StartPoint;
    if (name == "EndPoint") return SketchSubElement::EndPoint;
    if (name == "CenterPoint") return SketchSubElement::CenterPoint;
    if (name == "SplinePoint") return SketchSubElement::SplinePoint;
    return std::nullopt;
}

// --- enum <-> string (serializer-owned; enum headers stay minimal) ---------

std::string_view toString(UnitType unit) {
    switch (unit) {
        case UnitType::Unitless: return "Unitless";
        case UnitType::Millimeter: return "Millimeter";
        case UnitType::Radian: return "Radian";
        case UnitType::Kilogram: return "Kilogram";
        case UnitType::Second: return "Second";
        case UnitType::KilogramPerCubicMeter: return "KilogramPerCubicMeter";
    }
    return "Unitless";
}

std::optional<UnitType> unitTypeFromString(std::string_view text) {
    if (text == "Unitless") return UnitType::Unitless;
    if (text == "Millimeter") return UnitType::Millimeter;
    if (text == "Radian") return UnitType::Radian;
    if (text == "Kilogram") return UnitType::Kilogram;
    if (text == "Second") return UnitType::Second;
    if (text == "KilogramPerCubicMeter") return UnitType::KilogramPerCubicMeter;
    return std::nullopt;
}

std::string_view toString(ParameterState state) {
    switch (state) {
        case ParameterState::Valid: return "Valid";
        case ParameterState::Dirty: return "Dirty";
        case ParameterState::Failed: return "Failed";
    }
    return "Valid";
}

std::optional<ParameterState> parameterStateFromString(std::string_view text) {
    if (text == "Valid") return ParameterState::Valid;
    if (text == "Dirty") return ParameterState::Dirty;
    if (text == "Failed") return ParameterState::Failed;
    return std::nullopt;
}











std::string_view toString(ComputeState state) {
    switch (state) {
        case ComputeState::Valid: return "Valid";
        case ComputeState::Dirty: return "Dirty";
        case ComputeState::Failed: return "Failed";
        case ComputeState::Suppressed: return "Suppressed";
    }
    return "Valid";
}

std::optional<ComputeState> computeStateFromString(std::string_view text) {
    if (text == "Valid") return ComputeState::Valid;
    if (text == "Dirty") return ComputeState::Dirty;
    if (text == "Failed") return ComputeState::Failed;
    if (text == "Suppressed") return ComputeState::Suppressed;
    return std::nullopt;
}

// --- id <-> decimal string --------------------------------------------------



// One face query as a JSON object (v19, M17.14).
//
// Shared by the sketch's `trackedFace` and by an EdgesOfFace selection,
// because they hold the same thing -- and two encodings of one query is two
// formats that can drift apart inside a single file.
JsonValue WriteFaceQuery(const FaceQuery& query) {
    JsonValue entry = JsonValue::makeObject();
    if (query.createdBy.has_value())
        entry.set("createdBy", JsonValue::makeString(idToString(*query.createdBy)));
    const auto direction = [&entry](const char* key, Vec3 v) {
        JsonValue value = JsonValue::makeObject();
        value.set("x", JsonValue::makeNumber(v.x));
        value.set("y", JsonValue::makeNumber(v.y));
        value.set("z", JsonValue::makeNumber(v.z));
        entry.set(key, std::move(value));
    };
    if (query.extremeTowards.has_value()) direction("extremeTowards", *query.extremeTowards);
    if (query.facing.has_value()) direction("facing", *query.facing);
    return entry;
}

// One edge query as a JSON object (v18, M17.12).
//
// By NAME, because the loader dispatches on it -- and a query written under a
// name the loader does not know is not a forward-compatibility problem, it is
// a fillet that silently reverts to every edge.
JsonValue WriteEdgeQuery(const EdgeQuery& query) {
    JsonValue entry = JsonValue::makeObject();
    const auto direction = [&entry](Vec3 v) {
        entry.set("x", JsonValue::makeNumber(v.x));
        entry.set("y", JsonValue::makeNumber(v.y));
        entry.set("z", JsonValue::makeNumber(v.z));
    };
    // EXHAUSTIVE by std::visit with if-constexpr, not an if/else chain ending
    // in a bare `else`. The chain version was written when the variant had
    // three alternatives; adding a fourth made the `else` reach for the wrong
    // one and throw on save. A visit fails to COMPILE when an alternative has
    // nowhere to go, which is where that failure belongs.
    std::visit(
        [&entry, &direction](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, AllEdges>) {
                entry.set("type", JsonValue::makeString("AllEdges"));
            } else if constexpr (std::is_same_v<T, EdgesOfExtremeFace>) {
                entry.set("type", JsonValue::makeString("EdgesOfExtremeFace"));
                direction(value.direction);
            } else if constexpr (std::is_same_v<T, EdgesParallelTo>) {
                entry.set("type", JsonValue::makeString("EdgesParallelTo"));
                direction(value.direction);
            } else if constexpr (std::is_same_v<T, EdgesCreatedBy>) {
                entry.set("type", JsonValue::makeString("EdgesCreatedBy"));
                entry.set("featureId", JsonValue::makeString(idToString(value.featureId)));
            } else {
                static_assert(std::is_same_v<T, EdgesOfFace>);
                entry.set("type", JsonValue::makeString("EdgesOfFace"));
                entry.set("face", WriteFaceQuery(value.face));
            }
        },
        query);
    return entry;
}




// --- save -------------------------------------------------------------------

JsonValue toJson(const PartDocument& document) {
    JsonValue root = JsonValue::makeObject();
    root.set("format", JsonValue::makeString(std::string(kFormatName)));
    root.set("schemaVersion", JsonValue::makeNumber(kSchemaVersion));
    root.set("documentType", JsonValue::makeString("Part"));
    root.set("id", JsonValue::makeString(idToString(document.id())));
    root.set("name", JsonValue::makeString(document.name()));

    JsonValue parameters = JsonValue::makeArray();
    for (const auto& parameter : document.parameters().items()) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("id", JsonValue::makeString(idToString(parameter->id())));
        entry.set("name", JsonValue::makeString(parameter->name()));
        entry.set("value", JsonValue::makeNumber(parameter->value()));
        entry.set("unit", JsonValue::makeString(std::string(toString(parameter->unit()))));
        entry.set("expression", JsonValue::makeString(parameter->expression()));
        entry.set("state", JsonValue::makeString(std::string(toString(parameter->state()))));
        parameters.add(std::move(entry));
    }
    root.set("parameters", std::move(parameters));

    // v51 (M51). WHAT THE PART IS MADE OF, when it is made of sheet.
    //
    // Written only when it IS sheet metal, so an ordinary solid's file does
    // not carry a thickness of nothing that a later reader could mistake for a
    // stated one.
    if (document.sheetMetal().isSheetMetal) {
        JsonValue sheet = JsonValue::makeObject();
        sheet.set("thicknessMm", JsonValue::makeNumber(document.sheetMetal().thicknessMm));
        sheet.set("material",
                  JsonValue::makeString(std::string(toString(document.sheetMetal().material))));
        sheet.set("defaultBendRadiusMm",
                  JsonValue::makeNumber(document.sheetMetal().defaultBendRadiusMm));
        root.set("sheetMetal", std::move(sheet));
    }

    // Material (v3, ADR-M3-005): a single optional document-level record,
    // null when no material is assigned.
    if (document.material()) {
        const Material& material = *document.material();
        JsonValue materialJson = JsonValue::makeObject();
        materialJson.set("id", JsonValue::makeString(idToString(material.id())));
        materialJson.set("name", JsonValue::makeString(material.name()));
        materialJson.set("densityKgPerM3", JsonValue::makeNumber(material.density()));
        materialJson.set("elasticModulusPa", JsonValue::makeNumber(material.elasticModulusPa));
        materialJson.set("poissonRatio", JsonValue::makeNumber(material.poissonRatio));
        materialJson.set("yieldStrengthPa", JsonValue::makeNumber(material.yieldStrengthPa));
        JsonValue contact = JsonValue::makeObject();
        contact.set("staticFriction", JsonValue::makeNumber(material.contact.staticFriction));
        contact.set("dynamicFriction", JsonValue::makeNumber(material.contact.dynamicFriction));
        contact.set("restitution", JsonValue::makeNumber(material.contact.restitution));
        materialJson.set("contact", std::move(contact));
        root.set("material", std::move(materialJson));
    } else {
        root.set("material", JsonValue::makeNull());
    }

    JsonValue bodies = JsonValue::makeArray();
    std::unordered_set<ObjectId> featureIds; // Option B edges: re-derived, never in "dependencies"
    for (const auto& body : document.bodies()) {
        JsonValue bodyEntry = JsonValue::makeObject();
        bodyEntry.set("id", JsonValue::makeString(idToString(body->id())));
        bodyEntry.set("name", JsonValue::makeString(body->name()));
        // v9 (ADR-M9-004): the rollback POSITION, which is document state --
        // where the user is looking is part of the document, and reopening a
        // part rolled back to step three at step three is the behaviour a user
        // expects. Written only when it hides something, so a document nobody
        // has rolled back is byte-identical to its v8 self apart from the
        // version number.
        //
        // A COUNT, not a feature id: a position before the first feature and a
        // position after the last one both have to be expressible and neither
        // names a feature. That is not the ADR-M4-004 violation it might look
        // like -- the count is not an identity, nothing references it, and the
        // features it hides keep their own ids and their own order.
        if (body->rollbackCut() < body->features().size())
            bodyEntry.set("rollback",
                          JsonValue::makeNumber(static_cast<double>(body->rollbackCut())));
        JsonValue features = JsonValue::makeArray();
        for (const auto& feature : body->features()) {
            featureIds.insert(feature->id());
            JsonValue featureEntry = JsonValue::makeObject();
            featureEntry.set("id", JsonValue::makeString(idToString(feature->id())));
            featureEntry.set("name", JsonValue::makeString(feature->name()));
            // Feature type dispatch is keyed by the typeName() virtual call
            // (ADR-M3-005) -- no dynamic_cast probing to determine WHICH type
            // this is. A dynamic_cast is still used below solely to reach the
            // already-known concrete type's extra accessors.
            featureEntry.set("type", JsonValue::makeString(std::string(feature->typeName())));
            featureEntry.set("state", JsonValue::makeString(std::string(toString(feature->state()))));
            if (const auto* box = dynamic_cast<const BoxFeature*>(feature.get())) {
                featureEntry.set("widthParameterId",
                                 JsonValue::makeString(idToString(box->widthParameterId())));
                featureEntry.set("heightParameterId",
                                 JsonValue::makeString(idToString(box->heightParameterId())));
                featureEntry.set("depthParameterId",
                                 JsonValue::makeString(idToString(box->depthParameterId())));
                featureEntry.set("materialId", JsonValue::makeString(idToString(box->materialId())));
            } else if (const auto* pad = dynamic_cast<const PadFeature*>(feature.get())) {
                // Semantic references only (ADR-M4-004): the Sketch, the Length
                // Parameter and the Material, each by ObjectId. No OCCT
                // topology, no index, no address is ever written.
                featureEntry.set("sketchId", JsonValue::makeString(idToString(pad->sketchId())));
                featureEntry.set("lengthParameterId",
                                 JsonValue::makeString(idToString(pad->lengthParameterId())));
                featureEntry.set("materialId", JsonValue::makeString(idToString(pad->materialId())));
            } else if (const auto* imported =
                           dynamic_cast<const ImportFeature*>(feature.get())) {
                // v28 (M22). A PATH, which is the first string a feature record
                // has ever carried -- and it is still a reference, just one to
                // something outside the document. What is NOT written is the
                // imported geometry: no topology ever crosses into this file
                // (ADR-M4-004), so the sentence is stored and answered again on
                // every rebuild.
                //
                // Written VERBATIM, including whether it is relative. Turning a
                // relative path absolute at save time would bind the document
                // to the machine that saved it, which is the opposite of what a
                // relative path was chosen for.
                featureEntry.set("path", JsonValue::makeString(imported->path()));
                featureEntry.set("materialId",
                                 JsonValue::makeString(idToString(imported->materialId())));
            } else if (const auto* boolean =
                           dynamic_cast<const BooleanFeature*>(feature.get())) {
                // v27 (M21). TWO operands, and the ORDER is written because
                // "A minus B" and "B minus A" are different parts.
                featureEntry.set("baseFeatureId",
                                 JsonValue::makeString(idToString(boolean->targetFeatureId())));
                featureEntry.set("toolFeatureId",
                                 JsonValue::makeString(idToString(boolean->toolFeatureId())));
                featureEntry.set("operation",
                                 JsonValue::makeString(
                                     BooleanOperationName(boolean->operation())));
                featureEntry.set("materialId",
                                 JsonValue::makeString(idToString(boolean->materialId())));
            } else if (const auto* circular =
                           dynamic_cast<const CircularPatternFeature*>(feature.get())) {
                // BEFORE the PatternFeature branch below, for the reason the
                // snapshot tests it first: a base class swallows the derived
                // one and writes the wrong record.
                featureEntry.set("baseFeatureId",
                                 JsonValue::makeString(idToString(circular->baseFeatureId())));
                featureEntry.set("frameId",
                                 JsonValue::makeString(idToString(circular->frameId())));
                featureEntry.set("countParameterId",
                                 JsonValue::makeString(idToString(circular->countParameterId())));
                featureEntry.set("spacingParameterId",
                                 JsonValue::makeString(idToString(circular->stepParameterId())));
                featureEntry.set("materialId",
                                 JsonValue::makeString(idToString(circular->materialId())));
            } else if (const auto* curve =
                           dynamic_cast<const CurvePatternFeature*>(feature.get())) {
                featureEntry.set("baseFeatureId",
                                 JsonValue::makeString(idToString(curve->baseFeatureId())));
                featureEntry.set("sketchId",
                                 JsonValue::makeString(idToString(curve->pathSketchId())));
                featureEntry.set("countParameterId",
                                 JsonValue::makeString(idToString(curve->countParameterId())));
                featureEntry.set("materialId",
                                 JsonValue::makeString(idToString(curve->materialId())));
            } else if (const auto* shell = dynamic_cast<const ShellFeature*>(feature.get())) {
                // v26 (M20). The faces are QUERIES, written as the sentences
                // they are -- never as an index, which is what a stored face
                // would be (ADR-M17-036).
                featureEntry.set("baseFeatureId",
                                 JsonValue::makeString(idToString(shell->baseFeatureId())));
                featureEntry.set("sizeParameterId",
                                 JsonValue::makeString(idToString(shell->thicknessParameterId())));
                JsonValue faces = JsonValue::makeArray();
                for (const FaceQuery& query : shell->openFaces())
                    faces.add(WriteFaceQuery(query));
                featureEntry.set("faceSelection", std::move(faces));
                featureEntry.set("materialId",
                                 JsonValue::makeString(idToString(shell->materialId())));
            } else if (const auto* draft = dynamic_cast<const DraftFeature*>(feature.get())) {
                featureEntry.set("baseFeatureId",
                                 JsonValue::makeString(idToString(draft->baseFeatureId())));
                featureEntry.set("sizeParameterId",
                                 JsonValue::makeString(idToString(draft->angleParameterId())));
                JsonValue faces = JsonValue::makeArray();
                for (const FaceQuery& query : draft->faces()) faces.add(WriteFaceQuery(query));
                featureEntry.set("faceSelection", std::move(faces));
                featureEntry.set("neutralFace", WriteFaceQuery(draft->neutralFace()));
                featureEntry.set("materialId",
                                 JsonValue::makeString(idToString(draft->materialId())));
            } else if (const auto* hole = dynamic_cast<const HoleFeature*>(feature.get())) {
                featureEntry.set("baseFeatureId",
                                 JsonValue::makeString(idToString(hole->baseFeatureId())));
                featureEntry.set("sketchId",
                                 JsonValue::makeString(idToString(hole->sketchId())));
                featureEntry.set("diameterParameterId",
                                 JsonValue::makeString(idToString(hole->diameterParameterId())));
                featureEntry.set("depthParameterId",
                                 JsonValue::makeString(idToString(hole->depthParameterId())));
                // v43 (M39). THE SENTENCE, not the numbers it stands for. The
                // drill size is looked up from the designation every time it
                // is needed, so writing 6.8 as well would put a second answer
                // in the file -- one that goes stale the moment a table is
                // corrected, and that nothing would ever compare.
                featureEntry.set("holeKind", JsonValue::makeString(NameOfHoleKind(hole->kind())));
                if (hole->screw().named()) {
                    JsonValue screw = JsonValue::makeObject();
                    screw.set("designation",
                              JsonValue::makeString(hole->screw().designation));
                    screw.set("tapped", JsonValue::makeBool(hole->screw().tapped));
                    screw.set("fit", JsonValue::makeString(NameOf(hole->screw().fit)));
                    featureEntry.set("screw", std::move(screw));
                }
                featureEntry.set("materialId",
                                 JsonValue::makeString(idToString(hole->materialId())));
            } else if (const auto* sweep = dynamic_cast<const SweepFeature*>(feature.get())) {
                // v25 (M19): TWO sketches, each by ObjectId. `sketchId` is the
                // SECTION -- the field every other sketch-consuming feature
                // already uses for the thing that decides the outline -- and
                // `pathSketchId` is the spine.
                featureEntry.set("sketchId",
                                 JsonValue::makeString(idToString(sweep->profileSketchId())));
                featureEntry.set("pathSketchId",
                                 JsonValue::makeString(idToString(sweep->pathSketchId())));
                featureEntry.set("materialId",
                                 JsonValue::makeString(idToString(sweep->materialId())));
            } else if (const auto* loft = dynamic_cast<const LoftFeature*>(feature.get())) {
                // v25 (M19): the sections AS AN ARRAY, because the ORDER is the
                // shape. Written in the order the feature holds them and read
                // back in the order the file holds them, with nothing between
                // that could sort them -- lofting A-B-C and A-C-B are different
                // solids.
                JsonValue sections = JsonValue::makeArray();
                for (const ObjectId sectionId : loft->sectionSketchIds())
                    sections.add(JsonValue::makeString(idToString(sectionId)));
                featureEntry.set("sectionSketchIds", std::move(sections));
                featureEntry.set("materialId",
                                 JsonValue::makeString(idToString(loft->materialId())));
            } else if (const auto* revolve = dynamic_cast<const RevolveFeature*>(feature.get())) {
                // v7 (ADR-M8-005): the axis is a SketchEntityId in the named
                // sketch, persisted as a decimal string exactly like every
                // entity id -- semantic, never positional.
                featureEntry.set("sketchId",
                                 JsonValue::makeString(idToString(revolve->sketchId())));
                featureEntry.set("axisEntityId",
                                 JsonValue::makeString(
                                     idToString(ToObjectId(revolve->axisEntityId()))));
                featureEntry.set("angleParameterId",
                                 JsonValue::makeString(idToString(revolve->angleParameterId())));
                featureEntry.set("materialId",
                                 JsonValue::makeString(idToString(revolve->materialId())));
            } else if (const auto* dress = dynamic_cast<const EdgeDressFeature*>(feature.get())) {
                // v8 (ADR-M8-006): Fillet and Chamfer share one record shape;
                // the "type" field is the discriminator, exactly as everywhere.
                featureEntry.set("baseFeatureId",
                                 JsonValue::makeString(idToString(dress->baseFeatureId())));
                featureEntry.set("sizeParameterId",
                                 JsonValue::makeString(idToString(dress->sizeParameterId())));
                // v18: WHICH edges (M17.12). Written only when it is not the
                // default, so every fillet anybody already has produces the
                // bytes v17 produced -- and "absent" and "every edge" mean the
                // same thing, with no third state for them to disagree about.
                if (!IsAllEdges(dress->edgeSelection())) {
                    JsonValue queries = JsonValue::makeArray();
                    for (const EdgeQuery& query : dress->edgeSelection())
                        queries.add(WriteEdgeQuery(query));
                    featureEntry.set("edgeSelection", std::move(queries));
                }
                featureEntry.set("materialId",
                                 JsonValue::makeString(idToString(dress->materialId())));
            } else if (const auto* pattern =
                           dynamic_cast<const PatternFeature*>(feature.get())) {
                // v10 (M10.6): the frame supplies the direction, and count and
                // spacing are Parameters -- a pattern whose count cannot be
                // driven is not parametric.
                featureEntry.set("baseFeatureId",
                                 JsonValue::makeString(idToString(pattern->baseFeatureId())));
                featureEntry.set("frameId",
                                 JsonValue::makeString(idToString(pattern->frameId())));
                featureEntry.set("countParameterId",
                                 JsonValue::makeString(idToString(pattern->countParameterId())));
                featureEntry.set(
                    "spacingParameterId",
                    JsonValue::makeString(idToString(pattern->spacingParameterId())));
                featureEntry.set("materialId",
                                 JsonValue::makeString(idToString(pattern->materialId())));
            } else if (const auto* transform =
                           dynamic_cast<const TransformFeature*>(feature.get())) {
                // Mirror. AFTER Pattern, for the reason the dress branch is
                // before its twins: a base-class test placed first swallows the
                // derived type and silently drops its fields.
                featureEntry.set("baseFeatureId",
                                 JsonValue::makeString(idToString(transform->baseFeatureId())));
                featureEntry.set("frameId",
                                 JsonValue::makeString(idToString(transform->frameId())));
                featureEntry.set("materialId",
                                 JsonValue::makeString(idToString(transform->materialId())));
            } else if (const auto* pocket = dynamic_cast<const PocketFeature*>(feature.get())) {
                // v6 (ADR-M8-001): the chain reference is the base feature's
                // ObjectId -- semantic, like every other reference here. The
                // restore path requires the base to appear EARLIER in this
                // body's feature array, which creation order guarantees; the
                // save-side counterpart of that rule lives in validateSaveable.
                featureEntry.set("baseFeatureId",
                                 JsonValue::makeString(idToString(pocket->baseFeatureId())));
                featureEntry.set("sketchId",
                                 JsonValue::makeString(idToString(pocket->sketchId())));
                featureEntry.set("depthParameterId",
                                 JsonValue::makeString(idToString(pocket->depthParameterId())));
                featureEntry.set("materialId",
                                 JsonValue::makeString(idToString(pocket->materialId())));
            }
            features.add(std::move(featureEntry));
        }
        bodyEntry.set("features", std::move(features));
        bodies.add(std::move(bodyEntry));
    }
    root.set("bodies", std::move(bodies));

    // Sketches (v4, ADR-M4-001/002). Entity ids are persisted as decimal
    // strings exactly like ObjectIds -- they come from the same generator, so
    // restore inherits the same collision safety. Storage POSITION is never
    // written: the id is the identity (ADR-M4-004).
    JsonValue sketches = JsonValue::makeArray();
    for (const auto& sketch : document.sketches()) {
        JsonValue sketchEntry = JsonValue::makeObject();
        sketchEntry.set("id", JsonValue::makeString(idToString(sketch->id())));
        sketchEntry.set("name", JsonValue::makeString(sketch->name()));

        const Transform3D& transform = sketch->frame().transform();
        JsonValue frame = JsonValue::makeObject();
        JsonValue translation = JsonValue::makeObject();
        translation.set("x", JsonValue::makeNumber(transform.translation.x));
        translation.set("y", JsonValue::makeNumber(transform.translation.y));
        translation.set("z", JsonValue::makeNumber(transform.translation.z));
        frame.set("translation", std::move(translation));
        JsonValue rotation = JsonValue::makeObject();
        rotation.set("w", JsonValue::makeNumber(transform.rotation.w));
        rotation.set("x", JsonValue::makeNumber(transform.rotation.x));
        rotation.set("y", JsonValue::makeNumber(transform.rotation.y));
        rotation.set("z", JsonValue::makeNumber(transform.rotation.z));
        frame.set("rotation", std::move(rotation));
        sketchEntry.set("frame", std::move(frame));
        // v10 (M10.2, ADR-M10-003): the OPTIONAL support frame. Written only
        // when the sketch has one, so a document with no frames is byte-
        // identical to its v9 self apart from the version number and the two
        // empty arrays. The embedded "frame" above stays either way -- it is
        // the fallback, not dead weight.
        if (sketch->supportFrameId() != kInvalidObjectId)
            sketchEntry.set("supportFrameId",
                            JsonValue::makeString(idToString(sketch->supportFrameId())));

        // v19: the FACE this sketch follows (M17.14). Written only when there
        // is one, so a sketch on a world plane is byte-identical to its v18
        // self -- and absent means "the embedded frame is the whole story",
        // which is what every sketch made before this said by omission.
        if (sketch->trackedFace().has_value()) {
            sketchEntry.set("trackedFace", WriteFaceQuery(*sketch->trackedFace()));
        }

        JsonValue entities = JsonValue::makeArray();
        for (const SketchEntity& entity : sketch->entities()) {
            JsonValue entry = JsonValue::makeObject();
            entry.set("id", JsonValue::makeString(idToString(ToObjectId(entity.id))));
            WriteSketchGeometry(entry, entity.geometry);
            // v14. Written only when TRUE, so a sketch with no construction
            // geometry produces the same bytes it did before the flag existed
            // -- the absent field and `false` mean the same thing, and there is
            // no third state for them to disagree about.
            if (entity.construction) entry.set("construction", JsonValue::makeBool(true));
            entities.add(std::move(entry));
        }
        sketchEntry.set("entities", std::move(entities));

        // v17: the projected reference underlay (M17.6, ADR-M17-029).
        //
        // Written ONLY when there is one, so a sketch with no underlay produces
        // the bytes v16 produced. It is persisted for the same reason the
        // sketch's plane is: both are frozen snapshots of the face the sketch
        // was made on, and a user who saves, reopens and wants to trace one
        // more edge would otherwise find the underlay gone with no way to get
        // it back short of deleting the sketch and starting again.
        if (!sketch->references().empty()) {
            JsonValue references = JsonValue::makeArray();
            for (const SketchReference& reference : sketch->references()) {
                JsonValue entry = JsonValue::makeObject();
                entry.set("id", JsonValue::makeString(idToString(ToObjectId(reference.id))));
                WriteSketchGeometry(entry, reference.geometry);
                references.add(std::move(entry));
            }
            sketchEntry.set("references", std::move(references));
        }

        // Constraints (v5, ADR-M5-001; spec 17). Persisted: constraint id,
        // kind, entity/sub-element targets and the bound Parameter's ObjectId.
        // NOT persisted, deliberately: solver variable indexes, residual
        // indexes, Jacobian layout, DOF, solve status or any backend state --
        // all of it is derived, and writing a variable index as if it were
        // identity is exactly the failure spec 17 forbids. A reloaded sketch
        // re-solves before anything reads it.
        JsonValue constraints = JsonValue::makeArray();
        for (const SketchConstraint& constraint : sketch->constraints()) {
            JsonValue entry = JsonValue::makeObject();
            entry.set("id", JsonValue::makeString(idToString(ToObjectId(constraint.id))));
            entry.set("type", JsonValue::makeString(ConstraintKindName(constraint.data)));

            const auto writeRef = [&entry](const char* key, const SketchElementRef& ref) {
                JsonValue target = JsonValue::makeObject();
                target.set("entityId", JsonValue::makeString(idToString(ToObjectId(ref.entityId))));
                target.set("subElement", JsonValue::makeString(subElementName(ref.subElement)));
                // WRITTEN ONLY FOR A SPLINE POINT, which is the only
                // sub-element it means anything for -- so every file written
                // before M17.30 is byte-identical, and absent reads as 0 which
                // is what they all meant.
                if (ref.subElement == SketchSubElement::SplinePoint)
                    target.set("index", JsonValue::makeNumber(ref.index));
                entry.set(key, std::move(target));
            };
            const auto writeEntity = [&entry](const char* key, SketchEntityId id) {
                entry.set(key, JsonValue::makeString(idToString(ToObjectId(id))));
            };

            std::visit(
                [&](const auto& c) {
                    using T = std::decay_t<decltype(c)>;
                    if constexpr (std::is_same_v<T, CoincidentConstraint>) {
                        writeRef("a", c.a);
                        writeRef("b", c.b);
                    } else if constexpr (std::is_same_v<T, HorizontalConstraint> ||
                                         std::is_same_v<T, VerticalConstraint>) {
                        writeEntity("line", c.line);
                    } else if constexpr (std::is_same_v<T, PointsHorizontalConstraint> ||
                                         std::is_same_v<T, PointsVerticalConstraint>) {
                        // UNORDERED, unlike the signed axis distances below:
                        // "these two share a v" is the same statement either
                        // way round, so nothing here needs to preserve which
                        // point the user picked first.
                        writeRef("a", c.a);
                        writeRef("b", c.b);
                    } else if constexpr (std::is_same_v<T, FixConstraint>) {
                        writeRef("target", c.target);
                    } else if constexpr (std::is_same_v<T, DistanceConstraint> ||
                                         std::is_same_v<T, HorizontalDistanceConstraint> ||
                                         std::is_same_v<T, VerticalDistanceConstraint>) {
                        // The ORDER of a and b is part of what these mean: the
                        // two axis-aligned kinds are signed, so swapping the
                        // pair negates the value (see HorizontalDistance-
                        // Constraint). Written as-is, never normalised.
                        writeRef("a", c.a);
                        writeRef("b", c.b);
                    } else if constexpr (std::is_same_v<T, LengthConstraint>) {
                        writeEntity("line", c.line);
                    } else if constexpr (std::is_same_v<T, RadiusConstraint> ||
                                         std::is_same_v<T, DiameterConstraint>) {
                        writeEntity("curve", c.curve);
                    } else if constexpr (std::is_same_v<T, AngleConstraint> ||
                                         std::is_same_v<T, ParallelConstraint> ||
                                         std::is_same_v<T, PerpendicularConstraint>) {
                        writeEntity("lineA", c.lineA);
                        writeEntity("lineB", c.lineB);
                    } else if constexpr (std::is_same_v<T, EqualConstraint>) {
                        writeEntity("a", c.a);
                        writeEntity("b", c.b);
                    } else if constexpr (std::is_same_v<T, ConcentricConstraint>) {
                        writeEntity("curveA", c.curveA);
                        writeEntity("curveB", c.curveB);
                    } else if constexpr (std::is_same_v<T, MidpointConstraint>) {
                        writeRef("point", c.point);
                        writeEntity("line", c.line);
                    } else if constexpr (std::is_same_v<T, PointLineDistanceConstraint>) {
                        writeRef("point", c.point);
                        writeEntity("line", c.line);
                    } else if constexpr (std::is_same_v<T, SymmetricConstraint>) {
                        writeRef("a", c.a);
                        writeRef("b", c.b);
                        writeEntity("line", c.line);
                    } else if constexpr (std::is_same_v<T, PointOnObjectConstraint>) {
                        writeRef("point", c.point);
                        writeEntity("target", c.target);
                    } else if constexpr (std::is_same_v<T, EllipseRotationConstraint>) {
                        writeEntity("curve", c.curve);
                    } else if constexpr (std::is_same_v<T, EllipseAxisConstraint>) {
                        writeEntity("curve", c.curve);
                        // ALWAYS, including when false -- WHICH axis is part of
                        // what the constraint means, not a default. A file that
                        // omitted it would load as a dimension on the other
                        // axis, which is a different model rather than a
                        // missing field.
                        entry.set("minor", JsonValue::makeBool(c.minor));
                    } else {
                        static_assert(std::is_same_v<T, TangentConstraint>);
                        writeEntity("a", c.a);
                        writeEntity("b", c.b);
                        // Written ALWAYS, including when false. The branch is
                        // part of what the constraint MEANS (see
                        // TangentConstraint), so a file that omitted it would
                        // load as a different model rather than as a default.
                        entry.set("internal", JsonValue::makeBool(c.internal));
                        // v21: WHERE they touch, when that is known.
                        //
                        // Written only when it IS known, unlike `internal`
                        // above -- and the difference is not taste. `internal`
                        // has no defensible default, so its absence is a
                        // corrupt file. `at` has one: every tangency written
                        // before v21 meant a line free to slide, which is
                        // exactly what Whole says. Omitting it keeps those
                        // files byte-identical and reloads them as what they
                        // were, not as something new.
                        if (c.at != SketchSubElement::Whole)
                            entry.set("at", JsonValue::makeString(subElementName(c.at)));
                    }
                },
                constraint.data);

            const ObjectId parameterId = BoundParameterId(constraint.data);
            if (parameterId != kInvalidObjectId)
                entry.set("parameterId", JsonValue::makeString(idToString(parameterId)));
            // v20: a REFERENCE dimension measures instead of driving (M17.19).
            // Written only when true, so every file written before this is
            // byte-identical -- and absent means driving, which is what they
            // all meant.
            if (constraint.driven) entry.set("driven", JsonValue::makeBool(true));
            constraints.add(std::move(entry));
        }
        sketchEntry.set("constraints", std::move(constraints));

        // Where the user dragged each dimension's value. Written ONLY for the
        // dimensions that were actually moved: an automatically placed
        // dimension has no position to preserve, and writing one would freeze
        // today's layout rule into every file.
        JsonValue placements = JsonValue::makeArray();
        for (const Sketch::DimensionPlacement& placement : sketch->dimensionPlacements()) {
            JsonValue entry = JsonValue::makeObject();
            entry.set("constraintId",
                      JsonValue::makeString(idToString(ToObjectId(placement.constraintId))));
            entry.set("u", JsonValue::makeNumber(placement.labelMm.x));
            entry.set("v", JsonValue::makeNumber(placement.labelMm.y));
            placements.add(std::move(entry));
        }
        sketchEntry.set("dimensionPlacements", std::move(placements));

        // How each dimension's value reads. Same rule as placements: written
        // only where it differs from the default, so a plain drawing carries
        // no format records at all.
        JsonValue formats = JsonValue::makeArray();
        for (const Sketch::DimensionFormat& format : sketch->dimensionFormats()) {
            JsonValue entry = JsonValue::makeObject();
            entry.set("constraintId",
                      JsonValue::makeString(idToString(ToObjectId(format.constraintId))));
            entry.set("prefix", JsonValue::makeString(format.prefix));
            entry.set("suffix", JsonValue::makeString(format.suffix));
            entry.set("plusTolerance", JsonValue::makeNumber(format.plusTolerance));
            entry.set("minusTolerance", JsonValue::makeNumber(format.minusTolerance));
            formats.add(std::move(entry));
        }
        sketchEntry.set("dimensionFormats", std::move(formats));
        sketches.add(std::move(sketchEntry));
    }
    root.set("sketches", std::move(sketches));

    // ADR-012/ADR-M3-005: persist explicit Option-A edges whose BOTH
    // endpoints are persisted document objects and whose dependent is NOT a
    // Feature (Feature-owned edges, and every MassPropertiesNode edge, are
    // Option B: re-derived from semantic id fields, never written here).
    // Edges touching runtime-only recomputables (test stubs) are never
    // saved. Written in graph insertion order for deterministic output.
    std::unordered_set<ObjectId> persistedIds;
    for (const auto& parameter : document.parameters().items())
        persistedIds.insert(parameter->id());
    for (const auto& body : document.bodies()) {
        persistedIds.insert(body->id());
        for (const auto& feature : body->features())
            persistedIds.insert(feature->id());
    }
    JsonValue dependencies = JsonValue::makeArray();
    const DependencyGraph& graph = document.dependencyGraph();
    for (ObjectId prerequisite : graph.nodes()) {
        if (persistedIds.count(prerequisite) == 0) continue;
        for (ObjectId dependent : graph.dependentsOf(prerequisite)) {
            if (persistedIds.count(dependent) == 0) continue;
            if (featureIds.count(dependent) != 0) continue; // Option B
            JsonValue edge = JsonValue::makeObject();
            edge.set("prerequisite", JsonValue::makeString(idToString(prerequisite)));
            edge.set("dependent", JsonValue::makeString(idToString(dependent)));
            dependencies.add(std::move(edge));
        }
    }
    // v10: frames and connectors (M10). Written by DocumentJson since M23,
    // because they belong to DocumentBase and therefore to every document type
    // -- ADR-009 D6's "re-created fresh on load" is superseded, because a frame
    // the user moved has to survive a save, and a re-created Origin would
    // silently discard that move.
    root.set("frames", docjson::framesToJson(document));
    root.set("connectors", docjson::connectorsToJson(document));


    root.set("dependencies", std::move(dependencies));
    return root;
}

// --- load helpers -----------------------------------------------------------



// The inverse of WriteSketchGeometry, sharing its keys by construction.
//
// Returns false and fills `err` rather than throwing, matching the style of
// every other reader here. Extracted for the same reason as the writer: the
// references array (M17.6) is read with this exact parser, so the two arrays
// cannot come to disagree about what a "Line" is.
bool ReadSketchGeometry(const JsonValue& entry, const std::string& context, FieldError& err,
                        SketchGeometry& out) {
    const JsonValue* typeField = requireField(entry, "type", JsonType::String, context, err);
    if (typeField == nullptr) return false;
    const std::string type = typeField->asString();

    double u = 0.0, v = 0.0, u2 = 0.0, v2 = 0.0, radius = 0.0;
    double startAngle = 0.0, endAngle = 0.0;
    const auto num = [&](const char* key, double& value) -> bool {
        const JsonValue* field = requireField(entry, key, JsonType::Number, context, err);
        if (field == nullptr) return false;
        value = field->asNumber();
        return true;
    };

    if (type == "Point") {
        if (!num("u", u) || !num("v", v)) return false;
        out = SketchPoint{Vec2{u, v}};
        return true;
    }
    if (type == "Line") {
        if (!num("u1", u) || !num("v1", v) || !num("u2", u2) || !num("v2", v2)) return false;
        out = SketchLine{Vec2{u, v}, Vec2{u2, v2}};
        return true;
    }
    if (type == "Circle") {
        if (!num("u", u) || !num("v", v) || !num("radiusMm", radius)) return false;
        out = SketchCircle{Vec2{u, v}, radius};
        return true;
    }
    if (type == "Arc") {
        if (!num("u", u) || !num("v", v) || !num("radiusMm", radius) ||
            !num("startAngleRad", startAngle) || !num("endAngleRad", endAngle))
            return false;
        const JsonValue* ccw = requireField(entry, "counterClockwise", JsonType::Bool, context,
                                            err);
        if (ccw == nullptr) return false;
        out = SketchArc{Vec2{u, v}, radius, startAngle, endAngle, ccw->asBool()};
        return true;
    }
    if (type == "Spline") {
        const JsonValue* points = requireField(entry, "points", JsonType::Array, context, err);
        if (points == nullptr) return false;
        SketchSpline spline;
        std::size_t index = 0;
        for (const JsonValue& one : points->items()) {
            const std::string where = context + ".points[" + std::to_string(index++) + "]";
            if (one.type() != JsonType::Object) {
                err = fieldError(SerializationError::InvalidFieldType, where + ": not an object");
                return false;
            }
            const JsonValue* pu = requireField(one, "u", JsonType::Number, where, err);
            if (pu == nullptr) return false;
            const JsonValue* pv = requireField(one, "v", JsonType::Number, where, err);
            if (pv == nullptr) return false;
            spline.points.push_back(Vec2{pu->asNumber(), pv->asNumber()});
        }
        const JsonValue* closed = requireField(entry, "closed", JsonType::Bool, context, err);
        if (closed == nullptr) return false;
        spline.closed = closed->asBool();
        // OPTIONAL, because every file written before v24 has none. Present and
        // malformed is an ERROR, though: a handle that quietly failed to load
        // would give the reader a different curve through the same points,
        // which is the one thing a handle exists to make different.
        if (const JsonValue* handles = entry.find("handles")) {
            if (handles->type() != JsonType::Array) {
                err = fieldError(SerializationError::InvalidFieldType,
                                 context + ".handles: not an array");
                return false;
            }
            std::size_t at = 0;
            for (const JsonValue& one : handles->items()) {
                const std::string where = context + ".handles[" + std::to_string(at++) + "]";
                if (one.type() != JsonType::Object) {
                    err = fieldError(SerializationError::InvalidFieldType,
                                     where + ": not an object");
                    return false;
                }
                const JsonValue* which = requireField(one, "point", JsonType::Number, where, err);
                if (which == nullptr) return false;
                const JsonValue* du = requireField(one, "du", JsonType::Number, where, err);
                if (du == nullptr) return false;
                const JsonValue* dv = requireField(one, "dv", JsonType::Number, where, err);
                if (dv == nullptr) return false;
                const double raw = which->asNumber();
                if (raw < 0.0 || raw != std::floor(raw) ||
                    raw >= static_cast<double>(spline.points.size())) {
                    err = fieldError(SerializationError::InvalidFieldType,
                                     where + ".point: not one of this spline's points");
                    return false;
                }
                spline.handles[static_cast<int>(raw)] = Vec2{du->asNumber(), dv->asNumber()};
            }
        }
        out = std::move(spline);
        return true;
    }
    if (type == "Ellipse" || type == "EllipticalArc") {
        double major = 0.0, minor = 0.0, rotation = 0.0;
        if (!num("u", u) || !num("v", v) || !num("majorRadiusMm", major) ||
            !num("minorRadiusMm", minor) || !num("rotationRad", rotation))
            return false;
        if (type == "Ellipse") {
            out = SketchEllipse{Vec2{u, v}, major, minor, rotation};
            return true;
        }
        double startParam = 0.0, endParam = 0.0;
        if (!num("startParamRad", startParam) || !num("endParamRad", endParam)) return false;
        const JsonValue* ccw = requireField(entry, "counterClockwise", JsonType::Bool, context,
                                            err);
        if (ccw == nullptr) return false;
        out = SketchEllipticalArc{Vec2{u, v},  major,      minor,
                                  rotation,    startParam, endParam,
                                  ccw->asBool()};
        return true;
    }
    err = fieldError(SerializationError::InvalidEnumValue,
                     context + ": unknown sketch entity type '" + type + "'");
    return false;
}

// The inverse of WriteFaceQuery, shared by the sketch's trackedFace and by an
// EdgesOfFace selection for the same reason the writer is.
//
// An EMPTY query is refused: it matches every face, so it names none, and a
// holder of one would fail on every recompute from now on with nothing to fix.
bool ReadFaceQuery(const JsonValue& entry, const std::string& context, FieldError& err,
                   FaceQuery& out) {
    if (entry.type() != JsonType::Object) {
        err = fieldError(SerializationError::InvalidFieldType, context + ": not an object");
        return false;
    }
    FaceQuery query;
    if (const JsonValue* owner = entry.find("createdBy")) {
        if (owner->type() != JsonType::String) {
            err = fieldError(SerializationError::InvalidFieldType,
                             context + ".createdBy is not a string");
            return false;
        }
        const auto ownerId = idFromString(owner->asString());
        if (!ownerId || *ownerId == kInvalidObjectId || *ownerId > kMaxObjectId) {
            err = fieldError(SerializationError::InvalidFieldType,
                             context + ".createdBy is not a valid ObjectId");
            return false;
        }
        query.createdBy = *ownerId;
    }
    const auto readDirection = [&](const char* key, std::optional<Vec3>& into) -> bool {
        const JsonValue* field = entry.find(key);
        if (field == nullptr) return true; // absent is fine; this is a conjunction
        const JsonValue* x = field->type() == JsonType::Object ? field->find("x") : nullptr;
        const JsonValue* y = field->type() == JsonType::Object ? field->find("y") : nullptr;
        const JsonValue* z = field->type() == JsonType::Object ? field->find("z") : nullptr;
        if (x == nullptr || y == nullptr || z == nullptr || x->type() != JsonType::Number ||
            y->type() != JsonType::Number || z->type() != JsonType::Number) {
            err = fieldError(SerializationError::InvalidFieldType,
                             context + "." + key + " needs numeric x, y and z");
            return false;
        }
        into = Vec3{x->asNumber(), y->asNumber(), z->asNumber()};
        return true;
    };
    if (!readDirection("extremeTowards", query.extremeTowards)) return false;
    if (!readDirection("facing", query.facing)) return false;
    if (query.empty()) {
        err = fieldError(SerializationError::InvalidFieldType, context + " names no face");
        return false;
    }
    out = query;
    return true;
}

// A whole SELECTION of them (M20), through the one reader above -- so a shell's
// faces and a draft's are checked by the same rules the sketch's tracked face
// is, including the refusal of a query that names every face.
bool ReadFaceSelection(const JsonValue& entry, const char* name, const std::string& context,
                       FieldError& err, FaceSelection& out) {
    const JsonValue* array = requireField(entry, name, JsonType::Array, context, err);
    if (array == nullptr) return false;
    std::size_t at = 0;
    for (const JsonValue& one : array->items()) {
        FaceQuery query;
        if (!ReadFaceQuery(one, context + "." + name + "[" + std::to_string(at++) + "]", err,
                           query))
            return false;
        out.push_back(std::move(query));
    }
    return true;
}

// The inverse of WriteEdgeQuery. Returns false and fills `err` rather than
// throwing, like every other reader here.
bool ReadEdgeQuery(const JsonValue& entry, const std::string& context, FieldError& err,
                   EdgeQuery& out) {
    const JsonValue* typeField = requireField(entry, "type", JsonType::String, context, err);
    if (typeField == nullptr) return false;
    const std::string type = typeField->asString();
    if (type == "AllEdges") {
        out = AllEdges{};
        return true;
    }

    // Read BEFORE the direction fields, because this query has none: asking
    // for x/y/z first would refuse a perfectly good record for missing
    // something it never had.
    if (type == "EdgesOfFace") {
        const JsonValue* faceField = entry.find("face");
        if (faceField == nullptr) {
            err = fieldError(SerializationError::MissingField, context + ": missing 'face'");
            return false;
        }
        FaceQuery face;
        if (!ReadFaceQuery(*faceField, context + ".face", err, face)) return false;
        out = EdgesOfFace{face};
        return true;
    }
    if (type == "EdgesCreatedBy") {
        const JsonValue* idField =
            requireField(entry, "featureId", JsonType::String, context, err);
        if (idField == nullptr) return false;
        const auto featureId = idFromString(idField->asString());
        if (!featureId || *featureId > kMaxObjectId) {
            err = fieldError(SerializationError::InvalidFieldType,
                             context + ".featureId: not a valid decimal ObjectId string");
            return false;
        }
        out = EdgesCreatedBy{*featureId};
        return true;
    }

    Vec3 direction{};
    const auto num = [&](const char* key, double& value) -> bool {
        const JsonValue* field = requireField(entry, key, JsonType::Number, context, err);
        if (field == nullptr) return false;
        value = field->asNumber();
        return true;
    };
    if (!num("x", direction.x) || !num("y", direction.y) || !num("z", direction.z)) return false;

    if (type == "EdgesOfExtremeFace") {
        out = EdgesOfExtremeFace{direction};
        return true;
    }
    if (type == "EdgesParallelTo") {
        out = EdgesParallelTo{direction};
        return true;
    }
    err = fieldError(SerializationError::InvalidEnumValue,
                     context + ": unknown edge query type '" + type + "'");
    return false;
}



std::optional<ObjectId> requireIdField(const JsonValue& object, const std::string& context,
                                       FieldError& err) {
    const JsonValue* field = requireField(object, "id", JsonType::String, context, err);
    if (field == nullptr) return std::nullopt;
    const auto id = idFromString(field->asString());
    if (!id.has_value() || *id == kInvalidObjectId) {
        err = fieldError(SerializationError::InvalidFieldType,
                         context + ": field 'id' is not a valid decimal ObjectId string");
        return std::nullopt;
    }
    if (*id > kMaxObjectId) {
        err = fieldError(SerializationError::InvalidFieldType,
                         context + ": field 'id' value " + field->asString() +
                             " exceeds the maximum ObjectId (2^63 - 1)");
        return std::nullopt;
    }
    return id;
}

LoadResult loadFailure(SerializationError error, std::string message) {
    return LoadResult{nullptr, error, std::move(message)};
}

// The ONE table of solid-feature type names (M8 round 2, R2R2-M2/R2-R1-M1).
// The save side decides "is this a solid" by the ISolidFeature CAPABILITY;
// the load side has only type strings, so it needs a name list -- and when
// that list lived inline in the chain walk, adding a name to it (or omitting
// the next milestone's solid type) drifted silently past all 761 tests: the
// refusal just moved to a later layer, past the id-generator advance. Both
// the loader's chain walk and the reserved-typename save check now consult
// THIS table and nothing else; M8_REV_322 pins that every name here
// round-trips as a concrete chain base, so dropping one fails a test the day
// it happens. When a milestone adds an ISolidFeature type, its name goes
// here, and only here.
constexpr std::string_view kSolidFeatureTypeNames[] = {"Box",     "Pad",     "Pocket",
                                                       "Revolve", "Fillet",  "Chamfer",
                                                       "Mirror",  "Pattern", "Sweep",
                                                       "Loft",    "Shell",   "Draft",
                                                       "Hole",    "Boolean", "CircularPattern",
                                                       "CurvePattern", "Import"};

bool IsSolidFeatureTypeName(std::string_view name) {
    for (const std::string_view solid : kSolidFeatureTypeNames)
        if (name == solid) return true;
    return false;
}

// FUTURE-DIVERGENCE note (round 3, R2R3-m1): today every concrete type is a
// solid, so this one table serves both "is a legal chain base" (chain walk)
// and "is a reserved concrete name" (placeholder save check). The day a
// concrete NON-solid type ships (a datum, a reference feature), the two roles
// split: its name belongs in the reserved check but NOT the chain walk --
// introduce a separate kConcreteTypeNames then, rather than putting it here.

// The CONSUMER frontier, same discipline (round 3, R1/R2 minors): the loader
// decides "does this type consume a base" by name; the save side asks the
// consumedSolidId() capability. An inline || chain here was the same drift
// shape the solid table fixed. Each member's loader checks are pinned:
// Pocket by M8_SER_003, Fillet by M8_SER_203, Chamfer by M8_SER_205,
// uniqueness by M8_REV_304.
//
// "Fillet/Chamfer by M8_SER_203" is what this comment said until round 4, and
// it was FALSE -- 203 swaps Pad and Fillet, nothing swapped Fillet and Chamfer,
// and dropping "Chamfer" survived every shipped test. Both R1 and R2 found it
// independently (R1R4-M2 / R2R4-M2). It is R3R3-M1's finding -- a table pinned
// for only some of its members, with a comment claiming all of them --
// reproduced inside the commit that fixed it, in the table that commit added.
//
// So: when a milestone adds a consuming type, its name goes here AND it gets a
// test that puts its record BEFORE the base it consumes. A member with no such
// test is not pinned, whatever this comment says.
constexpr std::string_view kConsumingFeatureTypeNames[] = {"Pocket", "Fillet", "Chamfer",
                                                          "Mirror", "Pattern"};

bool IsConsumingFeatureTypeName(std::string_view name) {
    for (const std::string_view consumer : kConsumingFeatureTypeNames)
        if (name == consumer) return true;
    return false;
}

// Save/load symmetry guard: anything savePartDocument accepts must be
// loadable. The load path rejects a BoxFeature whose widthParameterId /
// heightParameterId / depthParameterId is not a parameter in the file, but the
// save path used to write those ids unchecked -- so removing a Parameter that a
// BoxFeature still references (reachable through the public
// PartDocument::removeObject) produced a file that saved cleanly and then
// failed to load forever, with the only copy of the data already overwritten.
//
// Failing the save instead surfaces the dangling reference while the in-memory
// document is still intact and repairable. The message deliberately mirrors the
// load-side wording so the two report the same defect recognisably.
SaveResult validateSaveable(const PartDocument& document) {
    // NO id may exceed the cap the LOADER enforces.
    //
    // `ObjectIdGenerator::AdvancePast` clamps to kMaxObjectId and then adds one,
    // so a document that restores an id of 2^63-1 leaves the process counter at
    // 2^63. Every id issued after that is above the cap this file's own loader
    // rejects -- so the save succeeded, and the file could never be opened
    // again. Independent review found `ASSERT_TRUE(savePartDocument(...))`
    // passing and the very next `loadPartDocument` refusing the bytes it had
    // just written.
    //
    // Refused here for the same reason as the placeholder case below: a save
    // that emits an unopenable document is worse than a save that fails, because
    // the user finds out later and has nothing left to recover from.
    //
    // ObjectId.h's claim of "2^63 organic allocations of headroom" was wrong --
    // the SAVABLE headroom is zero -- and is corrected there.
    const auto capCheck = [](ObjectId id, const char* what) -> SaveResult {
        if (id <= kMaxObjectId) return SaveResult{};
        return SaveResult{SerializationError::InvalidFieldType,
                          std::string(what) + " has id " + idToString(id) +
                              ", above the maximum this format can load (2^63 - 1); the "
                              "resulting file could never be loaded back"};
    };
    // The LOADER's stable-identity net, mirrored (M8 round 3, R1R3-M1's
    // second half): every persistent id unique across document / parameters /
    // material / bodies / features / sketches. The loader has enforced this
    // since M2; the save side never did, so any path that constructs an
    // in-memory duplicate -- the unguarded placeholder restore was the
    // demonstrated one -- saved cleanly and produced a file the loader
    // refuses. The facade guards make duplicates unconstructible today; this
    // net is the ADR-M3-008 backstop for every FUTURE unregistered type, and
    // is recorded as masked-by-design in the review doc (no current route
    // reaches it).
    std::unordered_set<ObjectId> seenIds;
    const auto uniqueCheck = [&seenIds](ObjectId id, const char* what) -> SaveResult {
        if (seenIds.insert(id).second) return SaveResult{};
        return SaveResult{SerializationError::DuplicateId,
                          std::string(what) + ": duplicate ObjectId " + idToString(id) +
                              " already used elsewhere in this document; the resulting "
                              "file could never be loaded back"};
    };
    if (const SaveResult bad = capCheck(document.id(), "the document"); !bad) return bad;
    if (const SaveResult bad = uniqueCheck(document.id(), "the document"); !bad) return bad;
    for (const auto& parameter : document.parameters().items()) {
        if (const SaveResult bad = capCheck(parameter->id(), "a parameter"); !bad) return bad;
        if (const SaveResult bad = uniqueCheck(parameter->id(), "a parameter"); !bad) return bad;
    }
    // Every stored expression must still resolve (M11.2).
    //
    // The facade refuses an expression that does not, and refuses to delete a
    // parameter another expression reads -- so no current route produces one.
    // This is the ADR-M3-008 backstop, the same shape as the id-uniqueness net
    // above: a save that emits a document this loader would refuse is worse
    // than a save that fails, because the user finds out later and has nothing
    // left to recover from.
    if (const PartDocument::ExpressionWiringResult bad =
            document.validateParameterExpressions();
        !bad.ok) {
        return SaveResult{SerializationError::UnknownDependencyId,
                          bad.message +
                              "; the resulting file could never be loaded back"};
    }
    for (const auto& body : document.bodies()) {
        if (const SaveResult bad = capCheck(body->id(), "a body"); !bad) return bad;
        if (const SaveResult bad = uniqueCheck(body->id(), "a body"); !bad) return bad;
        for (const auto& feature : body->features()) {
            if (const SaveResult bad = capCheck(feature->id(), "a feature"); !bad) return bad;
            if (const SaveResult bad = uniqueCheck(feature->id(), "a feature"); !bad) return bad;
        }
    }
    for (const Sketch* sketch : document.sketches()) {
        if (const SaveResult bad = capCheck(sketch->id(), "a sketch"); !bad) return bad;
        if (const SaveResult bad = uniqueCheck(sketch->id(), "a sketch"); !bad) return bad;
        // Entity and constraint ids are SKETCH-SCOPED, so they get their own
        // per-sketch sets rather than joining the document-wide one (M7 round
        // 2, R2-M2): the loader enforces exactly this scope, and folding them
        // into the document set would refuse files the loader accepts --
        // asymmetry in the other direction, which is no better.
        std::unordered_set<ObjectId> sketchEntityIds;
        std::unordered_set<ObjectId> sketchConstraintIds;
        for (const SketchEntity& entity : sketch->entities()) {
            if (const SaveResult bad = capCheck(ToObjectId(entity.id), "a sketch entity"); !bad)
                return bad;
            if (!sketchEntityIds.insert(ToObjectId(entity.id)).second)
                return SaveResult{SerializationError::DuplicateId,
                                  "sketch " + idToString(sketch->id()) +
                                      ": duplicate entity id " + idToString(ToObjectId(entity.id)) +
                                      "; the resulting file could never be loaded back"};
        }
        for (const SketchConstraint& constraint : sketch->constraints()) {
            if (const SaveResult bad = capCheck(ToObjectId(constraint.id), "a sketch constraint");
                !bad)
                return bad;
            if (!sketchConstraintIds.insert(ToObjectId(constraint.id)).second)
                return SaveResult{SerializationError::DuplicateId,
                                  "sketch " + idToString(sketch->id()) +
                                      ": duplicate constraint id " +
                                      idToString(ToObjectId(constraint.id)) +
                                      "; the resulting file could never be loaded back"};
        }
    }
    if (document.material() != nullptr) {
        if (const SaveResult bad = capCheck(document.material()->id(), "the material"); !bad)
            return bad;
        if (const SaveResult bad = uniqueCheck(document.material()->id(), "the material"); !bad)
            return bad;
    }

    std::unordered_set<ObjectId> parameterIds;
    for (const auto& parameter : document.parameters().items())
        parameterIds.insert(parameter->id());

    // A PlaceholderFeature preserves an unrecognized type string losslessly
    // (ADR-009 D4). That becomes a save/load asymmetry the moment a later
    // milestone introduces a concrete type with the SAME name: the record would
    // be written with type "Pad" but none of a Pad's required fields, and the
    // loader -- which now knows Pad -- would reject the file forever. Caught by
    // the M3 regression suite when M4 added PadFeature.
    //
    // Rejecting here rather than degrading on load is deliberate: the opposite
    // choice would let a real Pad whose fields were lost reload as an inert
    // placeholder, silently dropping the solid.
    for (const auto& body : document.bodies()) {
        for (const auto& feature : body->features()) {
            // ASK WHAT IT IS, do not enumerate what it is not (ADR-M3-007).
            //
            // This was a list of five `dynamic_cast`s -- every concrete type
            // known at the time -- and M10.6 walked straight into it: adding
            // MirrorFeature made a perfectly real Mirror look like "a
            // placeholder carrying a reserved name", and a document containing
            // one could not be saved. That is the drift shape review rounds 3
            // and 4 each found in a TABLE, here in a cast chain, and it is a
            // FIFTH registration site ADR-M9-006's list of four did not name.
            //
            // The check only ever meant "a PlaceholderFeature whose type name
            // collides with a reserved one", so it now asks exactly that. One
            // question that cannot drift, instead of a list that has to grow
            // with every feature type anyone adds.
            if (dynamic_cast<const PlaceholderFeature*>(feature.get()) == nullptr) continue;
            const std::string_view typeName = feature->typeName();
            if (!IsSolidFeatureTypeName(typeName)) continue;
            return SaveResult{SerializationError::InvalidFieldType,
                              "feature " + idToString(feature->id()) + " (" + feature->name() +
                                  ") is a placeholder carrying the reserved type name '" +
                                  std::string(typeName) +
                                  "'; the resulting file could never be loaded back"};
        }
    }

    std::unordered_set<ObjectId> sketchIds;
    for (const Sketch* sketch : document.sketches()) sketchIds.insert(sketch->id());

    // A consuming feature must reference a base that is an earlier SOLID
    // feature of the same body, and no base may be consumed twice (ADR-M3-008:
    // a file the loader would reject must never be savable; ADR-M8-001's chain
    // rule, which round 1 found silently violated by a diamond -- two pockets
    // consuming one pad saved, loaded, displayed two overlapping solids, and
    // weighed only one). "Earlier" matters because restore replays features in
    // array order, so a consumer restored before its base would wire an edge
    // to a node that does not exist yet. The consumed base is asked for
    // through the consumedSolidId() capability, never a concrete-type
    // enumeration (ADR-M3-007) -- the enumeration is what let the material
    // rule lapse when Pad was added.
    for (const auto& body : document.bodies()) {
        std::unordered_set<ObjectId> earlierSolids;
        std::unordered_set<ObjectId> consumedBases;
        for (const auto& feature : body->features()) {
            const auto* solid = dynamic_cast<const ISolidFeature*>(feature.get());
            // EVERY operand (M21). This read the singular consumedSolidId, so a
            // boolean's TOOL was checked by neither rule: a file could name a
            // tool from another body, or a tool an earlier feature had already
            // eaten, and save cleanly. The plural is the whole fix, and it is
            // one line rather than a second branch for two-operand features --
            // which is what a second branch would have become.
            for (const ObjectId consumedBase :
                 solid != nullptr ? solid->consumedSolidIds() : std::vector<ObjectId>{}) {
                if (consumedBase == kInvalidObjectId) continue;
                if (earlierSolids.count(consumedBase) == 0)
                    return SaveResult{SerializationError::UnknownDependencyId,
                                      "feature " + idToString(feature->id()) + " (" +
                                          feature->name() +
                                          "): its base feature " + idToString(consumedBase) +
                                          " is not an earlier solid feature of the same body; "
                                          "the resulting file could never be loaded back"};
                if (!consumedBases.insert(consumedBase).second)
                    return SaveResult{SerializationError::UnknownDependencyId,
                                      "feature " + idToString(feature->id()) + " (" +
                                          feature->name() +
                                          "): its base feature " + idToString(consumedBase) +
                                          " is already consumed by an earlier feature; a solid "
                                          "may be consumed once (ADR-M8-008)"};
            }
            if (solid != nullptr) earlierSolids.insert(feature->id());
        }
    }

    // Material references, checked for every referring feature type at once.
    // Enumerating concrete types here is what let this rule lapse when Pad was
    // added, so it asks the feature for its capability instead (ADR-M3-007).
    const ObjectId documentMaterialId =
        document.material() ? document.material()->id() : kInvalidObjectId;
    for (const auto& body : document.bodies()) {
        for (const auto& feature : body->features()) {
            const auto* referencing = dynamic_cast<const IMaterialReferencing*>(feature.get());
            if (referencing == nullptr) continue;
            const ObjectId referenced = referencing->materialId();
            if (referenced == kInvalidObjectId) continue; // no material assigned
            if (referenced == documentMaterialId) continue;
            return SaveResult{SerializationError::UnknownDependencyId,
                              "feature " + idToString(feature->id()) + " (" + feature->name() +
                                  "): materialId " + idToString(referenced) +
                                  " does not match this document's material"};
        }
    }

    for (const auto& body : document.bodies()) {
        for (const auto& feature : body->features()) {
            if (const auto* box = dynamic_cast<const BoxFeature*>(feature.get())) {
                for (ObjectId referenced : {box->widthParameterId(), box->heightParameterId(),
                                            box->depthParameterId()}) {
                    if (parameterIds.count(referenced) != 0) continue;
                    return SaveResult{SerializationError::UnknownDependencyId,
                                      "feature " + idToString(box->id()) + " (" + box->name() +
                                          "): box parameter id " + idToString(referenced) +
                                          " is not a parameter in this document"};
                }
            } else if (const auto* pad = dynamic_cast<const PadFeature*>(feature.get())) {
                // The same rule the loader enforces, applied here so a document
                // whose Pad lost its Sketch or Length can never be written to
                // disk over the last good copy (ADR-M3-008). Extending this
                // check alongside every new referencing feature type is the
                // point of the rule, and it was missed when Pad was added.
                if (parameterIds.count(pad->lengthParameterId()) == 0)
                    return SaveResult{SerializationError::UnknownDependencyId,
                                      "feature " + idToString(pad->id()) + " (" + pad->name() +
                                          "): pad length parameter id " +
                                          idToString(pad->lengthParameterId()) +
                                          " is not a parameter in this document"};
                if (sketchIds.count(pad->sketchId()) == 0)
                    return SaveResult{SerializationError::UnknownDependencyId,
                                      "feature " + idToString(pad->id()) + " (" + pad->name() +
                                          "): pad sketch id " + idToString(pad->sketchId()) +
                                          " is not a sketch in this document"};
            } else if (const auto* pocket = dynamic_cast<const PocketFeature*>(feature.get())) {
                // The M8 types below repeat Pad's lesson verbatim: every
                // reference the LOADER rejects, checked at save time. Round 1
                // (R2-C1) demonstrated all six gaps as save-OK -> load-refused
                // through the public facade -- ADR-M3-008's named worst class,
                // fourth recurrence. The messages mirror the loader's wording.
                if (parameterIds.count(pocket->depthParameterId()) == 0)
                    return SaveResult{SerializationError::UnknownDependencyId,
                                      "feature " + idToString(pocket->id()) + " (" +
                                          pocket->name() + "): pocket depth parameter id " +
                                          idToString(pocket->depthParameterId()) +
                                          " is not a parameter in this document"};
                if (sketchIds.count(pocket->sketchId()) == 0)
                    return SaveResult{SerializationError::UnknownDependencyId,
                                      "feature " + idToString(pocket->id()) + " (" +
                                          pocket->name() + "): pocket sketch id " +
                                          idToString(pocket->sketchId()) +
                                          " is not a sketch in this document"};
            } else if (const auto* shell = dynamic_cast<const ShellFeature*>(feature.get())) {
                // ADR-M3-008 again: every reference the LOADER resolves is
                // checked HERE, or a document saves cleanly and refuses to load.
                if (parameterIds.count(shell->thicknessParameterId()) == 0)
                    return SaveResult{SerializationError::UnknownDependencyId,
                                      "feature " + idToString(shell->id()) + " (" +
                                          shell->name() + "): shell thickness parameter id " +
                                          idToString(shell->thicknessParameterId()) +
                                          " is not a parameter in this document"};
            } else if (const auto* draft = dynamic_cast<const DraftFeature*>(feature.get())) {
                if (parameterIds.count(draft->angleParameterId()) == 0)
                    return SaveResult{SerializationError::UnknownDependencyId,
                                      "feature " + idToString(draft->id()) + " (" +
                                          draft->name() + "): draft angle parameter id " +
                                          idToString(draft->angleParameterId()) +
                                          " is not a parameter in this document"};
            } else if (const auto* hole = dynamic_cast<const HoleFeature*>(feature.get())) {
                // v43 (M39). VALIDATED AT SAVE BECAUSE THE LOADER CHECKS IT
                // (ADR-M3-008). A document that writes a designation its own
                // loader will reject is the named worst case: it saves
                // cleanly and will not reopen.
                if (hole->screw().named() && !MetricCoarseThread(hole->screw().designation))
                    return SaveResult{SerializationError::InvalidFieldType,
                                      "feature " + idToString(hole->id()) + " (" + hole->name() +
                                          "): '" + hole->screw().designation +
                                          "' is not a thread this build has numbers for"};
                if (sketchIds.count(hole->sketchId()) == 0)
                    return SaveResult{SerializationError::UnknownDependencyId,
                                      "feature " + idToString(hole->id()) + " (" + hole->name() +
                                          "): hole sketch id " + idToString(hole->sketchId()) +
                                          " is not a sketch in this document"};
                if (parameterIds.count(hole->diameterParameterId()) == 0)
                    return SaveResult{SerializationError::UnknownDependencyId,
                                      "feature " + idToString(hole->id()) + " (" + hole->name() +
                                          "): hole diameter parameter id " +
                                          idToString(hole->diameterParameterId()) +
                                          " is not a parameter in this document"};
                if (parameterIds.count(hole->depthParameterId()) == 0)
                    return SaveResult{SerializationError::UnknownDependencyId,
                                      "feature " + idToString(hole->id()) + " (" + hole->name() +
                                          "): hole depth parameter id " +
                                          idToString(hole->depthParameterId()) +
                                          " is not a parameter in this document"};
            } else if (const auto* sweep = dynamic_cast<const SweepFeature*>(feature.get())) {
                // The same lesson as Pad's, for both of a sweep's sketches
                // (ADR-M3-008): every reference the LOADER rejects is checked
                // HERE, or a document saves cleanly and refuses to load.
                if (sketchIds.count(sweep->profileSketchId()) == 0)
                    return SaveResult{SerializationError::UnknownDependencyId,
                                      "feature " + idToString(sweep->id()) + " (" +
                                          sweep->name() + "): sweep profile sketch id " +
                                          idToString(sweep->profileSketchId()) +
                                          " is not a sketch in this document"};
                if (sketchIds.count(sweep->pathSketchId()) == 0)
                    return SaveResult{SerializationError::UnknownDependencyId,
                                      "feature " + idToString(sweep->id()) + " (" +
                                          sweep->name() + "): sweep path sketch id " +
                                          idToString(sweep->pathSketchId()) +
                                          " is not a sketch in this document"};
            } else if (const auto* loft = dynamic_cast<const LoftFeature*>(feature.get())) {
                // EVERY section, because the loader resolves every one of them.
                for (const ObjectId sectionId : loft->sectionSketchIds())
                    if (sketchIds.count(sectionId) == 0)
                        return SaveResult{SerializationError::UnknownDependencyId,
                                          "feature " + idToString(loft->id()) + " (" +
                                              loft->name() + "): loft section sketch id " +
                                              idToString(sectionId) +
                                              " is not a sketch in this document"};
            } else if (const auto* revolve = dynamic_cast<const RevolveFeature*>(feature.get())) {
                if (parameterIds.count(revolve->angleParameterId()) == 0)
                    return SaveResult{SerializationError::UnknownDependencyId,
                                      "feature " + idToString(revolve->id()) + " (" +
                                          revolve->name() + "): revolve angle parameter id " +
                                          idToString(revolve->angleParameterId()) +
                                          " is not a parameter in this document"};
                const Sketch* revolveSketch = nullptr;
                for (const Sketch* sketch : document.sketches())
                    if (sketch->id() == revolve->sketchId()) revolveSketch = sketch;
                if (revolveSketch == nullptr)
                    return SaveResult{SerializationError::UnknownDependencyId,
                                      "feature " + idToString(revolve->id()) + " (" +
                                          revolve->name() + "): revolve sketch id " +
                                          idToString(revolve->sketchId()) +
                                          " is not a sketch in this document"};
                // The sharpest of the six (R2's probe A5): deleting any sketch
                // line that happens to be a revolve axis used to poison every
                // future save silently.
                if (revolveSketch->findEntity(revolve->axisEntityId()) == nullptr)
                    return SaveResult{SerializationError::UnknownDependencyId,
                                      "feature " + idToString(revolve->id()) + " (" +
                                          revolve->name() + "): revolve axis entity id " +
                                          idToString(ToObjectId(revolve->axisEntityId())) +
                                          " is not an entity of sketch " +
                                          idToString(revolve->sketchId())};
            } else if (const auto* dress = dynamic_cast<const EdgeDressFeature*>(feature.get())) {
                if (parameterIds.count(dress->sizeParameterId()) == 0)
                    return SaveResult{SerializationError::UnknownDependencyId,
                                      "feature " + idToString(dress->id()) + " (" +
                                          dress->name() +
                                          "): fillet/chamfer size parameter id " +
                                          idToString(dress->sizeParameterId()) +
                                          " is not a parameter in this document"};
            }
        }
    }

    // Constraint references (v5). Same rule as every reference check above: a
    // reference the loader would reject must never be writable to disk over the
    // last good copy (ADR-M3-008). The deletion policy (ADR-M5-009) is what
    // keeps this from ever firing in practice -- so if it DOES fire, a mutation
    // path bypassed the facade, and failing the save is how that surfaces
    // instead of producing a file that can never be loaded back.
    for (const Sketch* sketch : document.sketches()) {
        for (const SketchConstraint& constraint : sketch->constraints()) {
            for (SketchEntityId referenced : ReferencedEntities(constraint.data)) {
                if (sketch->findEntity(referenced) != nullptr) continue;
                return SaveResult{SerializationError::UnknownDependencyId,
                                  "sketch " + idToString(sketch->id()) + " constraint " +
                                      idToString(ToObjectId(constraint.id)) + " (" +
                                      ConstraintKindName(constraint.data) +
                                      "): references entity " +
                                      idToString(ToObjectId(referenced)) +
                                      " which is not in this sketch"};
            }
            const ObjectId boundParameter = BoundParameterId(constraint.data);
            // Mirror the LOADER, which requires 'parameterId' for all five
            // dimensional kinds. Skipping the check for an invalid id let a
            // document save successfully and then fail to load forever.
            if (boundParameter == kInvalidObjectId) {
                if (!IsDimensional(constraint.data)) continue;
                return SaveResult{SerializationError::UnknownDependencyId,
                                  "sketch " + idToString(sketch->id()) + " constraint " +
                                      idToString(ToObjectId(constraint.id)) + " (" +
                                      ConstraintKindName(constraint.data) +
                                      "): a dimensional constraint is bound to no Parameter, "
                                      "so the resulting file could never be loaded back"};
            }
            if (parameterIds.count(boundParameter) != 0) continue;
            return SaveResult{SerializationError::UnknownDependencyId,
                              "sketch " + idToString(sketch->id()) + " constraint " +
                                  idToString(ToObjectId(constraint.id)) + " (" +
                                  ConstraintKindName(constraint.data) + "): parameter id " +
                                  idToString(boundParameter) +
                                  " is not a parameter in this document"};
        }
    }

    // A sketch's support frame must exist (M10 gate I). Same rule, same reason
    // as every other reference checked here: the loader refuses a support id it
    // cannot resolve, so a save that wrote one would be ADR-M3-008's class
    // again -- and a document whose geometry has nowhere to live must not
    // overwrite the last good file.
    for (const Sketch* sketch : document.sketches()) {
        const ObjectId supportId = sketch->supportFrameId();
        if (supportId == kInvalidObjectId) continue;
        if (document.findFrame(supportId) != nullptr) continue;
        return SaveResult{SerializationError::UnknownDependencyId,
                          "sketch " + idToString(sketch->id()) + " (" + sketch->name() +
                              "): support frame id " + idToString(supportId) +
                              " is not a reference frame in this document"};
    }

    // The Option-A dependency edges, which nothing checked at all (round 4,
    // R2R4-C1 -- ADR-M3-008's named worst class, SIXTH recurrence).
    //
    // The asymmetry: the WRITER persists any edge whose endpoints are both
    // persisted ids {parameters, bodies, features} and whose DEPENDENT is not a
    // feature (feature-owned edges are Option B, re-derived from semantic id
    // fields). The LOADER accepts an endpoint only if it is a PARAMETER. So an
    // edge whose prerequisite is a FEATURE and whose dependent is a parameter --
    // `addDependency(parameterId, featureId)`, four public facade calls, no
    // private access -- was written by the saver and refused by the loader that
    // read it back. `validateSaveable`, extended in round 3 precisely as this
    // net, never walked the graph.
    //
    // Refused rather than silently dropped: dropping the edge would make save
    // and load both "succeed" and produce a DIFFERENT document, which is the
    // failure mode R2R4-m3 records for the other direction. Refusing is the
    // ADR-M3-008 contract -- a file the loader would reject is not savable.
    //
    // NOT a decision that such an edge is meaningless: a driven/reference
    // dimension (roadmap section 7) is precisely a Parameter that depends on
    // geometry, so the day EP3D has one, the LOADER grows to accept feature
    // endpoints and this check narrows with it. Until then the format cannot
    // carry it, and the honest answer is to say so at save time.
    {
        std::unordered_set<ObjectId> persistedIds = parameterIds;
        std::unordered_set<ObjectId> featureIds;
        for (const auto& body : document.bodies()) {
            persistedIds.insert(body->id());
            for (const auto& feature : body->features()) {
                persistedIds.insert(feature->id());
                featureIds.insert(feature->id());
            }
        }
        const DependencyGraph& graph = document.dependencyGraph();
        for (ObjectId prerequisite : graph.nodes()) {
            if (persistedIds.count(prerequisite) == 0) continue;
            for (ObjectId dependent : graph.dependentsOf(prerequisite)) {
                if (persistedIds.count(dependent) == 0) continue;
                if (featureIds.count(dependent) != 0) continue; // Option B, not written
                // Exactly the loader's acceptance rule, endpoint for endpoint.
                for (ObjectId endpointId : {prerequisite, dependent}) {
                    if (parameterIds.count(endpointId) != 0) continue;
                    return SaveResult{SerializationError::UnknownDependencyId,
                                      "dependency edge " + idToString(prerequisite) + " -> " +
                                          idToString(dependent) + ": id " +
                                          idToString(endpointId) +
                                          " is not a parameter, and this format's dependency "
                                          "records carry parameter endpoints only, so the "
                                          "resulting file could never be loaded back"};
                }
            }
        }
    }
    return SaveResult{};
}

} // namespace

// --- public API -------------------------------------------------------------

int CurrentSchemaVersion() noexcept { return kSchemaVersion; }

SaveResult savePartDocument(const PartDocument& document, std::ostream& out) {
    if (const SaveResult invalid = validateSaveable(document); !invalid) return invalid;
    const std::string text = writeJson(toJson(document));
    out << text << '\n';
    if (!out.good())
        return SaveResult{SerializationError::IoError, "failed to write document to stream"};
    return SaveResult{};
}

LoadResult loadPartDocument(std::istream& in) {
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad())
        return loadFailure(SerializationError::IoError, "failed to read document from stream");
    const std::string text = buffer.str();

    JsonParseError parseError;
    const JsonValue root = parseJson(text, parseError);
    if (!parseError.ok) {
        std::ostringstream message;
        message << "malformed JSON at line " << parseError.line << ", column "
                << parseError.column << ": " << parseError.message;
        return loadFailure(SerializationError::MalformedJson, message.str());
    }
    if (root.type() != JsonType::Object)
        return loadFailure(SerializationError::MalformedJson, "top-level JSON value is not an object");

    FieldError err;
    const std::string documentContext = "document";

    // Header validation.
    const JsonValue* format = requireField(root, "format", JsonType::String, documentContext, err);
    if (format == nullptr) return loadFailure(err.error, err.message);
    if (format->asString() != kFormatName)
        return loadFailure(SerializationError::WrongFormat,
                           "unrecognized format '" + format->asString() + "'");

    // The version is a CEILING, not a content gate (review round 1, R2-m2,
    // stating a choice that was previously implicit): a file stamped v6 that
    // carries a v8-only record type still loads, because every record is
    // validated by CONTENT below regardless of the stamp. Refusing "newer
    // records than the stamp admits" would add a second source of truth about
    // what the file contains, and the two could disagree; the stamp's one job
    // is to refuse files newer than this LOADER (the > kSchemaVersion check).
    const JsonValue* schemaVersion =
        requireField(root, "schemaVersion", JsonType::Number, documentContext, err);
    if (schemaVersion == nullptr) return loadFailure(err.error, err.message);
    const double schemaVersionValue = schemaVersion->asNumber();
    if (schemaVersionValue < static_cast<double>(kMinSupportedSchemaVersion) ||
        schemaVersionValue > static_cast<double>(kSchemaVersion) ||
        schemaVersionValue != static_cast<double>(static_cast<int>(schemaVersionValue))) {
        std::ostringstream message;
        message << "unsupported schema version " << schemaVersionValue;
        return loadFailure(SerializationError::UnsupportedSchemaVersion, message.str());
    }

    const JsonValue* documentType =
        requireField(root, "documentType", JsonType::String, documentContext, err);
    if (documentType == nullptr) return loadFailure(err.error, err.message);
    if (documentType->asString() != "Part")
        return loadFailure(SerializationError::WrongDocumentType,
                           "unsupported document type '" + documentType->asString() + "'");

    const auto documentId = requireIdField(root, documentContext, err);
    if (!documentId.has_value()) return loadFailure(err.error, err.message);

    // Stable-identity rule: every persistent id in a document is unique across
    // all categories (document, parameters, bodies, features).
    std::unordered_set<ObjectId> seenIds;
    const auto registerId = [&seenIds](ObjectId id, const std::string& context,
                                       FieldError& fieldErr) {
        if (!seenIds.insert(id).second) {
            fieldErr = fieldError(SerializationError::DuplicateId,
                                  context + ": duplicate ObjectId " + idToString(id) +
                                      " already used elsewhere in this document");
            return false;
        }
        return true;
    };
    if (!registerId(*documentId, documentContext, err))
        return loadFailure(err.error, err.message);
    const JsonValue* name = requireField(root, "name", JsonType::String, documentContext, err);
    if (name == nullptr) return loadFailure(err.error, err.message);

    const JsonValue* parameters =
        requireField(root, "parameters", JsonType::Array, documentContext, err);
    if (parameters == nullptr) return loadFailure(err.error, err.message);
    const JsonValue* bodies = requireField(root, "bodies", JsonType::Array, documentContext, err);
    if (bodies == nullptr) return loadFailure(err.error, err.message);

    // Validate everything before constructing the document so an error can
    // never leave a partially restored result (and never advances the id
    // generator for a document that fails to load).
    struct ParameterData {
        ObjectId id;
        std::string name;
        double value;
        UnitType unit;
        std::string expression;
        ParameterState state;
    };
    // The LOADER'S feature record IS FeatureSnapshot (M17.13, ADR-M17-035).
    //
    // There used to be a second struct here with the same eighteen fields, and
    // eighteen lines further down copying one into the other. That is this
    // project's oldest recurring defect wearing its plainest clothes: two
    // things that must agree, a hand-written copy between them, and each side
    // tested on its own. Adding a nineteenth field to one and not the other
    // loses it silently on load -- which is exactly how `boundary` was lost
    // between FacePlane and PickedFace, one milestone ago.
    //
    // One struct cannot disagree with itself, and it cannot forget a field
    // added to it later.
    using FeatureData = FeatureSnapshot;
    struct BodyData {
        ObjectId id;
        std::string name;
        std::vector<FeatureData> features;
        std::size_t rollback = Body::kNoRollback;
    };
    struct MaterialData {
        ObjectId id;
        std::string name;
        double densityKgPerM3;
        double elasticModulusPa;
        double poissonRatio;
        double yieldStrengthPa;
        ContactProperties contact;
    };

    std::vector<ParameterData> parameterData;
    for (std::size_t i = 0; i < parameters->items().size(); ++i) {
        const JsonValue& entry = parameters->items()[i];
        const std::string context = "parameters[" + std::to_string(i) + "]";
        if (entry.type() != JsonType::Object)
            return loadFailure(SerializationError::InvalidFieldType, context + ": entry is not an object");

        const auto id = requireIdField(entry, context, err);
        if (!id.has_value()) return loadFailure(err.error, err.message);
        if (!registerId(*id, context, err)) return loadFailure(err.error, err.message);
        const JsonValue* paramName = requireField(entry, "name", JsonType::String, context, err);
        if (paramName == nullptr) return loadFailure(err.error, err.message);
        const JsonValue* value = requireField(entry, "value", JsonType::Number, context, err);
        if (value == nullptr) return loadFailure(err.error, err.message);
        const JsonValue* unit = requireField(entry, "unit", JsonType::String, context, err);
        if (unit == nullptr) return loadFailure(err.error, err.message);
        const JsonValue* expression =
            requireField(entry, "expression", JsonType::String, context, err);
        if (expression == nullptr) return loadFailure(err.error, err.message);
        const JsonValue* state = requireField(entry, "state", JsonType::String, context, err);
        if (state == nullptr) return loadFailure(err.error, err.message);

        const auto unitValue = unitTypeFromString(unit->asString());
        if (!unitValue.has_value())
            return loadFailure(SerializationError::InvalidEnumValue,
                               context + ": unknown unit '" + unit->asString() + "'");
        const auto stateValue = parameterStateFromString(state->asString());
        if (!stateValue.has_value())
            return loadFailure(SerializationError::InvalidEnumValue,
                               context + ": unknown parameter state '" + state->asString() + "'");

        parameterData.push_back(ParameterData{*id, paramName->asString(), value->asNumber(),
                                              *unitValue, expression->asString(), *stateValue});
    }
    std::unordered_set<ObjectId> parameterIds;
    for (const auto& parameter : parameterData) parameterIds.insert(parameter.id);

    // Material (v3, ADR-M3-005): a single optional document-level record.
    // Parsed before bodies so BoxFeature.materialId references can be
    // validated against it.
    std::optional<MaterialData> materialData;
    const JsonValue* materialField = root.find("material");
    if (materialField != nullptr && materialField->type() != JsonType::Null) {
        const std::string context = "material";
        if (materialField->type() != JsonType::Object)
            return loadFailure(SerializationError::InvalidFieldType,
                               context + ": field has the wrong JSON type");
        const auto id = requireIdField(*materialField, context, err);
        if (!id.has_value()) return loadFailure(err.error, err.message);
        if (!registerId(*id, context, err)) return loadFailure(err.error, err.message);
        const JsonValue* materialName =
            requireField(*materialField, "name", JsonType::String, context, err);
        if (materialName == nullptr) return loadFailure(err.error, err.message);
        const JsonValue* density =
            requireField(*materialField, "densityKgPerM3", JsonType::Number, context, err);
        if (density == nullptr) return loadFailure(err.error, err.message);
        const JsonValue* elastic =
            requireField(*materialField, "elasticModulusPa", JsonType::Number, context, err);
        if (elastic == nullptr) return loadFailure(err.error, err.message);
        const JsonValue* poisson =
            requireField(*materialField, "poissonRatio", JsonType::Number, context, err);
        if (poisson == nullptr) return loadFailure(err.error, err.message);
        const JsonValue* yield =
            requireField(*materialField, "yieldStrengthPa", JsonType::Number, context, err);
        if (yield == nullptr) return loadFailure(err.error, err.message);
        const JsonValue* contactField =
            requireField(*materialField, "contact", JsonType::Object, context, err);
        if (contactField == nullptr) return loadFailure(err.error, err.message);
        const std::string contactContext = context + ".contact";
        const JsonValue* staticFriction = requireField(*contactField, "staticFriction",
                                                        JsonType::Number, contactContext, err);
        if (staticFriction == nullptr) return loadFailure(err.error, err.message);
        const JsonValue* dynamicFriction = requireField(*contactField, "dynamicFriction",
                                                         JsonType::Number, contactContext, err);
        if (dynamicFriction == nullptr) return loadFailure(err.error, err.message);
        const JsonValue* restitution = requireField(*contactField, "restitution", JsonType::Number,
                                                    contactContext, err);
        if (restitution == nullptr) return loadFailure(err.error, err.message);

        ContactProperties contact;
        contact.staticFriction = staticFriction->asNumber();
        contact.dynamicFriction = dynamicFriction->asNumber();
        contact.restitution = restitution->asNumber();
        materialData = MaterialData{*id,          materialName->asString(), density->asNumber(),
                                    elastic->asNumber(), poisson->asNumber(), yield->asNumber(),
                                    contact};
    }

    // FRAMES AND CONNECTORS, read by DocumentJson (M23, ADR-M23-003). They
    // belong to DocumentBase, so both document types read them with the same
    // code and the same messages; `registerId` is passed in because the SET of
    // ids that must be unique differs by document type while the rule does not.
    std::vector<docjson::FrameData> frameData;
    std::vector<docjson::ConnectorData> connectorData;
    if (!docjson::readFrames(root, registerId, err, frameData))
        return loadFailure(err.error, err.message);
    if (!docjson::readConnectors(root, registerId, err, connectorData))
        return loadFailure(err.error, err.message);

    std::vector<BodyData> bodyData;
    for (std::size_t i = 0; i < bodies->items().size(); ++i) {
        const JsonValue& entry = bodies->items()[i];
        const std::string context = "bodies[" + std::to_string(i) + "]";
        if (entry.type() != JsonType::Object)
            return loadFailure(SerializationError::InvalidFieldType, context + ": entry is not an object");

        const auto id = requireIdField(entry, context, err);
        if (!id.has_value()) return loadFailure(err.error, err.message);
        if (!registerId(*id, context, err)) return loadFailure(err.error, err.message);
        const JsonValue* bodyName = requireField(entry, "name", JsonType::String, context, err);
        if (bodyName == nullptr) return loadFailure(err.error, err.message);
        const JsonValue* features = requireField(entry, "features", JsonType::Array, context, err);
        if (features == nullptr) return loadFailure(err.error, err.message);

        BodyData body{*id, bodyName->asString(), {}, Body::kNoRollback};
        // Absent means "evaluate everything", which is what every file written
        // before v9 means and what most v9 files mean too.
        if (const JsonValue* rollback = entry.find("rollback")) {
            if (rollback->type() != JsonType::Number)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": field 'rollback' is not a number");
            const double value = rollback->asNumber();
            if (value < 0.0 || value != static_cast<double>(static_cast<long long>(value)))
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": field 'rollback' is not a whole count");
            body.rollback = static_cast<std::size_t>(value);
        }
        for (std::size_t j = 0; j < features->items().size(); ++j) {
            const JsonValue& featureEntry = features->items()[j];
            const std::string featureContext = context + ".features[" + std::to_string(j) + "]";
            if (featureEntry.type() != JsonType::Object)
                return loadFailure(SerializationError::InvalidFieldType,
                                   featureContext + ": entry is not an object");

            const auto featureId = requireIdField(featureEntry, featureContext, err);
            if (!featureId.has_value()) return loadFailure(err.error, err.message);
            if (!registerId(*featureId, featureContext, err))
                return loadFailure(err.error, err.message);
            const JsonValue* featureName =
                requireField(featureEntry, "name", JsonType::String, featureContext, err);
            if (featureName == nullptr) return loadFailure(err.error, err.message);
            const JsonValue* featureType =
                requireField(featureEntry, "type", JsonType::String, featureContext, err);
            if (featureType == nullptr) return loadFailure(err.error, err.message);
            const JsonValue* featureState =
                requireField(featureEntry, "state", JsonType::String, featureContext, err);
            if (featureState == nullptr) return loadFailure(err.error, err.message);

            const auto stateValue = computeStateFromString(featureState->asString());
            if (!stateValue.has_value())
                return loadFailure(SerializationError::InvalidEnumValue,
                                   featureContext + ": unknown feature state '" +
                                       featureState->asString() + "'");

            FeatureData featureData{*featureId, featureName->asString(), featureType->asString(),
                                    *stateValue};
            if (featureData.typeName == "Box") {
                // Feature type dispatch (which concrete type to construct) is
                // keyed by this string, not dynamic_cast probing
                // (ADR-M3-005).
                const JsonValue* widthField = requireField(featureEntry, "widthParameterId",
                                                           JsonType::String, featureContext, err);
                if (widthField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* heightField = requireField(featureEntry, "heightParameterId",
                                                            JsonType::String, featureContext, err);
                if (heightField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* depthField = requireField(featureEntry, "depthParameterId",
                                                           JsonType::String, featureContext, err);
                if (depthField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* materialIdField = requireField(featureEntry, "materialId",
                                                                JsonType::String, featureContext, err);
                if (materialIdField == nullptr) return loadFailure(err.error, err.message);

                const auto widthId = idFromString(widthField->asString());
                const auto heightId = idFromString(heightField->asString());
                const auto depthId = idFromString(depthField->asString());
                const auto boxMaterialId = idFromString(materialIdField->asString());
                if (!widthId || !heightId || !depthId || !boxMaterialId || *widthId > kMaxObjectId ||
                    *heightId > kMaxObjectId || *depthId > kMaxObjectId ||
                    *boxMaterialId > kMaxObjectId) {
                    return loadFailure(SerializationError::InvalidFieldType,
                                       featureContext +
                                           ": box parameter/material id is not a valid decimal "
                                           "ObjectId string");
                }
                for (ObjectId referenced : {*widthId, *heightId, *depthId}) {
                    if (parameterIds.count(referenced) == 0)
                        return loadFailure(SerializationError::UnknownDependencyId,
                                           featureContext + ": box parameter id " +
                                               idToString(referenced) +
                                               " is not a parameter in this document");
                }
                if (*boxMaterialId != kInvalidObjectId &&
                    (!materialData.has_value() || materialData->id != *boxMaterialId)) {
                    return loadFailure(SerializationError::UnknownDependencyId,
                                       featureContext + ": box materialId " +
                                           idToString(*boxMaterialId) +
                                           " does not match this document's material");
                }
                featureData.widthParameterId = *widthId;
                featureData.heightParameterId = *heightId;
                featureData.depthParameterId = *depthId;
                featureData.materialId = *boxMaterialId;
            } else if (featureData.typeName == "Pad") {
                const JsonValue* sketchField = requireField(featureEntry, "sketchId",
                                                            JsonType::String, featureContext, err);
                if (sketchField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* lengthField = requireField(
                    featureEntry, "lengthParameterId", JsonType::String, featureContext, err);
                if (lengthField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* padMaterialField = requireField(
                    featureEntry, "materialId", JsonType::String, featureContext, err);
                if (padMaterialField == nullptr) return loadFailure(err.error, err.message);

                const auto sketchRef = idFromString(sketchField->asString());
                const auto lengthRef = idFromString(lengthField->asString());
                const auto padMaterialId = idFromString(padMaterialField->asString());
                if (!sketchRef || !lengthRef || !padMaterialId || *sketchRef > kMaxObjectId ||
                    *lengthRef > kMaxObjectId || *padMaterialId > kMaxObjectId) {
                    return loadFailure(SerializationError::InvalidFieldType,
                                       featureContext +
                                           ": pad sketch/parameter/material id is not a valid "
                                           "decimal ObjectId string");
                }
                if (parameterIds.count(*lengthRef) == 0)
                    return loadFailure(SerializationError::UnknownDependencyId,
                                       featureContext + ": pad length parameter id " +
                                           idToString(*lengthRef) +
                                           " is not a parameter in this document");
                if (*padMaterialId != kInvalidObjectId &&
                    (!materialData.has_value() || materialData->id != *padMaterialId)) {
                    return loadFailure(SerializationError::UnknownDependencyId,
                                       featureContext + ": pad materialId " +
                                           idToString(*padMaterialId) +
                                           " does not match this document's material");
                }
                featureData.sketchId = *sketchRef;
                featureData.lengthParameterId = *lengthRef;
                featureData.materialId = *padMaterialId;
            } else if (featureData.typeName == "Pocket") {
                const JsonValue* baseField = requireField(featureEntry, "baseFeatureId",
                                                          JsonType::String, featureContext, err);
                if (baseField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* sketchField = requireField(featureEntry, "sketchId",
                                                            JsonType::String, featureContext, err);
                if (sketchField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* depthField = requireField(
                    featureEntry, "depthParameterId", JsonType::String, featureContext, err);
                if (depthField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* pocketMaterialField = requireField(
                    featureEntry, "materialId", JsonType::String, featureContext, err);
                if (pocketMaterialField == nullptr) return loadFailure(err.error, err.message);

                const auto baseRef = idFromString(baseField->asString());
                const auto sketchRef = idFromString(sketchField->asString());
                const auto depthRef = idFromString(depthField->asString());
                const auto pocketMaterialId = idFromString(pocketMaterialField->asString());
                if (!baseRef || !sketchRef || !depthRef || !pocketMaterialId ||
                    *baseRef > kMaxObjectId || *sketchRef > kMaxObjectId ||
                    *depthRef > kMaxObjectId || *pocketMaterialId > kMaxObjectId) {
                    return loadFailure(SerializationError::InvalidFieldType,
                                       featureContext +
                                           ": pocket base/sketch/parameter/material id is not a "
                                           "valid decimal ObjectId string");
                }
                if (parameterIds.count(*depthRef) == 0)
                    return loadFailure(SerializationError::UnknownDependencyId,
                                       featureContext + ": pocket depth parameter id " +
                                           idToString(*depthRef) +
                                           " is not a parameter in this document");
                if (*pocketMaterialId != kInvalidObjectId &&
                    (!materialData.has_value() || materialData->id != *pocketMaterialId)) {
                    return loadFailure(SerializationError::UnknownDependencyId,
                                       featureContext + ": pocket materialId " +
                                           idToString(*pocketMaterialId) +
                                           " does not match this document's material");
                }
                featureData.baseFeatureId = *baseRef;
                featureData.sketchId = *sketchRef;
                featureData.depthParameterId = *depthRef;
                featureData.materialId = *pocketMaterialId;
            } else if (featureData.typeName == "Import") {
                const JsonValue* pathField = requireField(featureEntry, "path", JsonType::String,
                                                          featureContext, err);
                if (pathField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* importMaterialField = requireField(
                    featureEntry, "materialId", JsonType::String, featureContext, err);
                if (importMaterialField == nullptr) return loadFailure(err.error, err.message);
                // AN EMPTY PATH names no file, so the feature it describes can
                // never build. Refused at the door rather than at recompute
                // time, where the reason would be a long way from the cause.
                if (pathField->asString().empty())
                    return loadFailure(SerializationError::InvalidFieldType,
                                       featureContext + ": an import names no file");
                featureData.importPath = pathField->asString();
                const auto importMaterialId = idFromString(importMaterialField->asString());
                if (!importMaterialId || *importMaterialId > kMaxObjectId)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       featureContext + ": import materialId is not a valid id");
                featureData.materialId = *importMaterialId;
            } else if (featureData.typeName == "Boolean") {
                const JsonValue* targetField = requireField(featureEntry, "baseFeatureId",
                                                            JsonType::String, featureContext, err);
                if (targetField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* toolField = requireField(featureEntry, "toolFeatureId",
                                                          JsonType::String, featureContext, err);
                if (toolField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* opField = requireField(featureEntry, "operation",
                                                        JsonType::String, featureContext, err);
                if (opField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* boolMaterialField = requireField(
                    featureEntry, "materialId", JsonType::String, featureContext, err);
                if (boolMaterialField == nullptr) return loadFailure(err.error, err.message);

                // BY NAME, and an unknown one is REFUSED rather than defaulted
                // to Union. A file that says "Intersct" describes a part this
                // build cannot make, and quietly unioning instead would load a
                // different solid without a word.
                const std::string opName = opField->asString();
                if (opName == "Union") featureData.booleanOperation = BooleanOperation::Union;
                else if (opName == "Subtract")
                    featureData.booleanOperation = BooleanOperation::Subtract;
                else if (opName == "Intersect")
                    featureData.booleanOperation = BooleanOperation::Intersect;
                else
                    return loadFailure(SerializationError::InvalidFieldType,
                                       featureContext + ": '" + opName +
                                           "' is not a boolean operation");

                const auto targetRef = idFromString(targetField->asString());
                const auto toolRef = idFromString(toolField->asString());
                const auto boolMaterialId = idFromString(boolMaterialField->asString());
                if (!targetRef || !toolRef || !boolMaterialId || *targetRef > kMaxObjectId ||
                    *toolRef > kMaxObjectId || *boolMaterialId > kMaxObjectId)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       featureContext + ": boolean references are not valid ids");
                featureData.baseFeatureId = *targetRef;
                featureData.toolFeatureId = *toolRef;
                featureData.materialId = *boolMaterialId;
            } else if (featureData.typeName == "CircularPattern") {
                const JsonValue* baseField = requireField(featureEntry, "baseFeatureId",
                                                          JsonType::String, featureContext, err);
                if (baseField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* frameField = requireField(featureEntry, "frameId",
                                                          JsonType::String, featureContext, err);
                if (frameField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* countField = requireField(featureEntry, "countParameterId",
                                                          JsonType::String, featureContext, err);
                if (countField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* stepField = requireField(featureEntry, "spacingParameterId",
                                                         JsonType::String, featureContext, err);
                if (stepField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* ringMaterialField = requireField(
                    featureEntry, "materialId", JsonType::String, featureContext, err);
                if (ringMaterialField == nullptr) return loadFailure(err.error, err.message);

                const auto baseRef = idFromString(baseField->asString());
                const auto frameRef = idFromString(frameField->asString());
                const auto countRef = idFromString(countField->asString());
                const auto stepRef = idFromString(stepField->asString());
                const auto ringMaterialId = idFromString(ringMaterialField->asString());
                if (!baseRef || !frameRef || !countRef || !stepRef || !ringMaterialId ||
                    *baseRef > kMaxObjectId || *frameRef > kMaxObjectId ||
                    *countRef > kMaxObjectId || *stepRef > kMaxObjectId ||
                    *ringMaterialId > kMaxObjectId)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       featureContext +
                                           ": circular pattern references are not valid ids");
                featureData.baseFeatureId = *baseRef;
                featureData.frameId = *frameRef;
                featureData.countParameterId = *countRef;
                featureData.spacingParameterId = *stepRef;
                featureData.materialId = *ringMaterialId;
            } else if (featureData.typeName == "CurvePattern") {
                const JsonValue* baseField = requireField(featureEntry, "baseFeatureId",
                                                          JsonType::String, featureContext, err);
                if (baseField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* pathField = requireField(featureEntry, "sketchId",
                                                          JsonType::String, featureContext, err);
                if (pathField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* countField = requireField(featureEntry, "countParameterId",
                                                          JsonType::String, featureContext, err);
                if (countField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* alongMaterialField = requireField(
                    featureEntry, "materialId", JsonType::String, featureContext, err);
                if (alongMaterialField == nullptr) return loadFailure(err.error, err.message);

                const auto baseRef = idFromString(baseField->asString());
                const auto pathRef = idFromString(pathField->asString());
                const auto countRef = idFromString(countField->asString());
                const auto alongMaterialId = idFromString(alongMaterialField->asString());
                if (!baseRef || !pathRef || !countRef || !alongMaterialId ||
                    *baseRef > kMaxObjectId || *pathRef > kMaxObjectId ||
                    *countRef > kMaxObjectId || *alongMaterialId > kMaxObjectId)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       featureContext +
                                           ": curve pattern references are not valid ids");
                featureData.baseFeatureId = *baseRef;
                featureData.sketchId = *pathRef;
                featureData.countParameterId = *countRef;
                featureData.materialId = *alongMaterialId;
            } else if (featureData.typeName == "Shell" ||
                       featureData.typeName == "Draft") {
                const bool isDraft = featureData.typeName == "Draft";
                const JsonValue* baseField = requireField(featureEntry, "baseFeatureId",
                                                          JsonType::String, featureContext, err);
                if (baseField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* sizeField = requireField(featureEntry, "sizeParameterId",
                                                          JsonType::String, featureContext, err);
                if (sizeField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* dressMaterialField = requireField(
                    featureEntry, "materialId", JsonType::String, featureContext, err);
                if (dressMaterialField == nullptr) return loadFailure(err.error, err.message);
                if (!ReadFaceSelection(featureEntry, "faceSelection", featureContext, err,
                                       featureData.faceSelection))
                    return loadFailure(err.error, err.message);
                // AT LEAST ONE, checked at the door. A shell with no opening is
                // a hollow with no way in and a draft with no face tapers
                // nothing; both are features this program will refuse to build,
                // so a file describing one is refused where the reason is near
                // the cause.
                if (featureData.faceSelection.empty())
                    return loadFailure(SerializationError::InvalidFieldType,
                                       featureContext + ": " + featureData.typeName +
                                           " names no faces");
                if (isDraft) {
                    const JsonValue* neutralField = requireField(
                        featureEntry, "neutralFace", JsonType::Object, featureContext, err);
                    if (neutralField == nullptr) return loadFailure(err.error, err.message);
                    if (!ReadFaceQuery(*neutralField, featureContext + ".neutralFace", err,
                                       featureData.neutralFace))
                        return loadFailure(err.error, err.message);
                }

                const auto baseRef = idFromString(baseField->asString());
                const auto sizeRef = idFromString(sizeField->asString());
                const auto dressMaterialId = idFromString(dressMaterialField->asString());
                if (!baseRef || !sizeRef || !dressMaterialId || *baseRef > kMaxObjectId ||
                    *sizeRef > kMaxObjectId || *dressMaterialId > kMaxObjectId)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       featureContext + ": " + featureData.typeName +
                                           " references are not valid ids");
                featureData.baseFeatureId = *baseRef;
                featureData.sizeParameterId = *sizeRef;
                featureData.materialId = *dressMaterialId;
            } else if (featureData.typeName == "Hole") {
                const JsonValue* baseField = requireField(featureEntry, "baseFeatureId",
                                                          JsonType::String, featureContext, err);
                if (baseField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* sketchField = requireField(featureEntry, "sketchId",
                                                            JsonType::String, featureContext, err);
                if (sketchField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* boreField = requireField(featureEntry, "diameterParameterId",
                                                          JsonType::String, featureContext, err);
                if (boreField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* deepField = requireField(featureEntry, "depthParameterId",
                                                          JsonType::String, featureContext, err);
                if (deepField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* holeMaterialField = requireField(
                    featureEntry, "materialId", JsonType::String, featureContext, err);
                if (holeMaterialField == nullptr) return loadFailure(err.error, err.message);

                const auto baseRef = idFromString(baseField->asString());
                const auto sketchRef = idFromString(sketchField->asString());
                const auto boreRef = idFromString(boreField->asString());
                const auto deepRef = idFromString(deepField->asString());
                const auto holeMaterialId = idFromString(holeMaterialField->asString());
                if (!baseRef || !sketchRef || !boreRef || !deepRef || !holeMaterialId ||
                    *baseRef > kMaxObjectId || *sketchRef > kMaxObjectId ||
                    *boreRef > kMaxObjectId || *deepRef > kMaxObjectId ||
                    *holeMaterialId > kMaxObjectId)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       featureContext + ": hole references are not valid ids");
                featureData.baseFeatureId = *baseRef;
                featureData.sketchId = *sketchRef;
                featureData.diameterParameterId = *boreRef;
                featureData.holeDepthParameterId = *deepRef;
                featureData.materialId = *holeMaterialId;

                // v43 (M39), and BOTH ARE OPTIONAL: a file written before M39
                // has neither, and a plain hole is exactly what it should come
                // back as. A missing field here is an older file, not a
                // damaged one.
                if (const JsonValue* kindField = featureEntry.find("holeKind")) {
                    if (kindField->type() != JsonType::String)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           featureContext + ": holeKind is not a string");
                    const std::optional<HoleKind> kind = HoleKindNamed(kindField->asString());
                    if (!kind)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           featureContext + ": '" + kindField->asString() +
                                               "' is not a hole shape this build knows");
                    featureData.holeKind = *kind;
                }
                if (const JsonValue* screwField = featureEntry.find("screw")) {
                    if (screwField->type() != JsonType::Object)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           featureContext + ": screw is not an object");
                    const JsonValue* designation = requireField(*screwField, "designation",
                                                                JsonType::String,
                                                                featureContext, err);
                    if (designation == nullptr) return loadFailure(err.error, err.message);
                    // A DESIGNATION THIS BUILD CANNOT SIZE IS REFUSED AT LOAD.
                    //
                    // ADR-M3-008: what the saver will not write, the loader
                    // will not read. Accepted here, it would come back as a
                    // hole that cannot recompute -- a document that opens and
                    // then fails, which is harder to explain than one that
                    // says why it will not open.
                    if (!MetricCoarseThread(designation->asString()))
                        return loadFailure(SerializationError::InvalidFieldType,
                                           featureContext + ": '" + designation->asString() +
                                               "' is not a thread this build has numbers for");
                    HoleScrew screw;
                    screw.designation = designation->asString();
                    if (const JsonValue* tapped = screwField->find("tapped")) {
                        if (tapped->type() != JsonType::Bool)
                            return loadFailure(SerializationError::InvalidFieldType,
                                               featureContext + ": tapped is not a boolean");
                        screw.tapped = tapped->asBool();
                    }
                    if (const JsonValue* fit = screwField->find("fit")) {
                        if (fit->type() != JsonType::String)
                            return loadFailure(SerializationError::InvalidFieldType,
                                               featureContext + ": fit is not a string");
                        const std::optional<ClearanceFit> named =
                            ClearanceFitNamed(fit->asString());
                        if (!named)
                            return loadFailure(SerializationError::InvalidFieldType,
                                               featureContext + ": '" + fit->asString() +
                                                   "' is not a clearance fit");
                        screw.fit = *named;
                    }
                    featureData.holeScrew = std::move(screw);
                }
            } else if (featureData.typeName == "Sweep") {
                const JsonValue* sectionField = requireField(featureEntry, "sketchId",
                                                             JsonType::String, featureContext, err);
                if (sectionField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* spineField = requireField(featureEntry, "pathSketchId",
                                                           JsonType::String, featureContext, err);
                if (spineField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* sweepMaterialField = requireField(
                    featureEntry, "materialId", JsonType::String, featureContext, err);
                if (sweepMaterialField == nullptr) return loadFailure(err.error, err.message);

                const auto sectionRef = idFromString(sectionField->asString());
                const auto spineRef = idFromString(spineField->asString());
                const auto sweepMaterialId = idFromString(sweepMaterialField->asString());
                if (!sectionRef || !spineRef || !sweepMaterialId || *sectionRef > kMaxObjectId ||
                    *spineRef > kMaxObjectId || *sweepMaterialId > kMaxObjectId)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       featureContext + ": sweep references are not valid ids");
                featureData.sketchId = *sectionRef;
                featureData.pathSketchId = *spineRef;
                featureData.materialId = *sweepMaterialId;
            } else if (featureData.typeName == "Loft") {
                const JsonValue* sectionsField = requireField(
                    featureEntry, "sectionSketchIds", JsonType::Array, featureContext, err);
                if (sectionsField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* loftMaterialField = requireField(
                    featureEntry, "materialId", JsonType::String, featureContext, err);
                if (loftMaterialField == nullptr) return loadFailure(err.error, err.message);

                // IN FILE ORDER, and nothing sorts them on the way in.
                for (const JsonValue& one : sectionsField->items()) {
                    if (one.type() != JsonType::String)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           featureContext +
                                               ": a loft section id is not a string");
                    const auto sectionRef = idFromString(one.asString());
                    if (!sectionRef || *sectionRef > kMaxObjectId)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           featureContext +
                                               ": a loft section id is not a valid id");
                    featureData.sectionSketchIds.push_back(*sectionRef);
                }
                // TWO OR MORE, checked at the door. A file claiming a
                // one-section loft describes a feature this program cannot
                // build, and letting it in would trade a clear load error for a
                // failed feature the user has to work out for themselves.
                if (featureData.sectionSketchIds.size() < 2)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       featureContext + ": a loft needs at least two sections");
                const auto loftMaterialId = idFromString(loftMaterialField->asString());
                if (!loftMaterialId || *loftMaterialId > kMaxObjectId)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       featureContext + ": loft materialId is not a valid id");
                featureData.materialId = *loftMaterialId;
            } else if (featureData.typeName == "Revolve") {
                const JsonValue* sketchField = requireField(featureEntry, "sketchId",
                                                            JsonType::String, featureContext, err);
                if (sketchField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* axisField = requireField(featureEntry, "axisEntityId",
                                                          JsonType::String, featureContext, err);
                if (axisField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* angleField = requireField(
                    featureEntry, "angleParameterId", JsonType::String, featureContext, err);
                if (angleField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* revolveMaterialField = requireField(
                    featureEntry, "materialId", JsonType::String, featureContext, err);
                if (revolveMaterialField == nullptr) return loadFailure(err.error, err.message);

                const auto sketchRef = idFromString(sketchField->asString());
                const auto axisRef = idFromString(axisField->asString());
                const auto angleRef = idFromString(angleField->asString());
                const auto revolveMaterialId = idFromString(revolveMaterialField->asString());
                if (!sketchRef || !axisRef || !angleRef || !revolveMaterialId ||
                    *sketchRef > kMaxObjectId || *axisRef > kMaxObjectId ||
                    *angleRef > kMaxObjectId || *revolveMaterialId > kMaxObjectId) {
                    return loadFailure(SerializationError::InvalidFieldType,
                                       featureContext +
                                           ": revolve sketch/axis/parameter/material id is not "
                                           "a valid decimal ObjectId string");
                }
                if (parameterIds.count(*angleRef) == 0)
                    return loadFailure(SerializationError::UnknownDependencyId,
                                       featureContext + ": revolve angle parameter id " +
                                           idToString(*angleRef) +
                                           " is not a parameter in this document");
                if (*revolveMaterialId != kInvalidObjectId &&
                    (!materialData.has_value() || materialData->id != *revolveMaterialId)) {
                    return loadFailure(SerializationError::UnknownDependencyId,
                                       featureContext + ": revolve materialId " +
                                           idToString(*revolveMaterialId) +
                                           " does not match this document's material");
                }
                featureData.sketchId = *sketchRef;
                featureData.axisEntityId = *axisRef;
                featureData.angleParameterId = *angleRef;
                featureData.materialId = *revolveMaterialId;
            } else if (featureData.typeName == "Mirror" || featureData.typeName == "Pattern") {
                // v10 (M10.6). Validated like every other reference the loader
                // reads: a base that is an earlier solid of this body (checked
                // by the chain walk), a FRAME that exists, and -- for a pattern
                // -- two Parameters. The save side mirrors all of it, so a file
                // that would be refused here is never written (ADR-M3-008).
                const JsonValue* baseField =
                    requireField(featureEntry, "baseFeatureId", JsonType::String, featureContext,
                                 err);
                if (baseField == nullptr) return loadFailure(err.error, err.message);
                const auto baseRef = idFromString(baseField->asString());
                if (!baseRef.has_value())
                    return loadFailure(SerializationError::InvalidFieldType,
                                       featureContext + ": 'baseFeatureId' is not an ObjectId");
                const JsonValue* frameField =
                    requireField(featureEntry, "frameId", JsonType::String, featureContext, err);
                if (frameField == nullptr) return loadFailure(err.error, err.message);
                const auto frameRef = idFromString(frameField->asString());
                if (!frameRef.has_value())
                    return loadFailure(SerializationError::InvalidFieldType,
                                       featureContext + ": 'frameId' is not an ObjectId");
                featureData.baseFeatureId = *baseRef;
                featureData.frameId = *frameRef;
                if (featureData.typeName == "Pattern") {
                    const JsonValue* countField = requireField(
                        featureEntry, "countParameterId", JsonType::String, featureContext, err);
                    if (countField == nullptr) return loadFailure(err.error, err.message);
                    const auto countRef = idFromString(countField->asString());
                    const JsonValue* spacingField =
                        requireField(featureEntry, "spacingParameterId", JsonType::String,
                                     featureContext, err);
                    if (spacingField == nullptr) return loadFailure(err.error, err.message);
                    const auto spacingRef = idFromString(spacingField->asString());
                    if (!countRef.has_value() || !spacingRef.has_value())
                        return loadFailure(SerializationError::InvalidFieldType,
                                           featureContext +
                                               ": a pattern parameter id is not an ObjectId");
                    if (parameterIds.count(*countRef) == 0 ||
                        parameterIds.count(*spacingRef) == 0)
                        return loadFailure(SerializationError::UnknownDependencyId,
                                           featureContext +
                                               ": a pattern parameter id is not a parameter in "
                                               "this document");
                    featureData.countParameterId = *countRef;
                    featureData.spacingParameterId = *spacingRef;
                }
                if (const JsonValue* materialField = featureEntry.find("materialId")) {
                    if (materialField->type() == JsonType::String) {
                        const auto materialRef = idFromString(materialField->asString());
                        if (materialRef.has_value()) featureData.materialId = *materialRef;
                    }
                }
            } else if (featureData.typeName == "Fillet" || featureData.typeName == "Chamfer") {
                const JsonValue* baseField = requireField(featureEntry, "baseFeatureId",
                                                          JsonType::String, featureContext, err);
                if (baseField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* sizeField = requireField(
                    featureEntry, "sizeParameterId", JsonType::String, featureContext, err);
                if (sizeField == nullptr) return loadFailure(err.error, err.message);
                const JsonValue* dressMaterialField = requireField(
                    featureEntry, "materialId", JsonType::String, featureContext, err);
                if (dressMaterialField == nullptr) return loadFailure(err.error, err.message);

                const auto baseRef = idFromString(baseField->asString());
                const auto sizeRef = idFromString(sizeField->asString());
                const auto dressMaterialId = idFromString(dressMaterialField->asString());
                if (!baseRef || !sizeRef || !dressMaterialId || *baseRef > kMaxObjectId ||
                    *sizeRef > kMaxObjectId || *dressMaterialId > kMaxObjectId) {
                    return loadFailure(SerializationError::InvalidFieldType,
                                       featureContext +
                                           ": fillet/chamfer base/parameter/material id is not "
                                           "a valid decimal ObjectId string");
                }
                if (parameterIds.count(*sizeRef) == 0)
                    return loadFailure(SerializationError::UnknownDependencyId,
                                       featureContext + ": fillet/chamfer size parameter id " +
                                           idToString(*sizeRef) +
                                           " is not a parameter in this document");
                if (*dressMaterialId != kInvalidObjectId &&
                    (!materialData.has_value() || materialData->id != *dressMaterialId)) {
                    return loadFailure(SerializationError::UnknownDependencyId,
                                       featureContext + ": fillet/chamfer materialId " +
                                           idToString(*dressMaterialId) +
                                           " does not match this document's material");
                }
                featureData.baseFeatureId = *baseRef;
                featureData.sizeParameterId = *sizeRef;

                // v18, OPTIONAL: absent means every edge, which is what every
                // file written before this says by omission. A wrong TYPE is
                // still refused -- reading a malformed selection as "all"
                // would turn a corrupt file into a quietly different solid.
                if (const JsonValue* queries = featureEntry.find("edgeSelection")) {
                    if (queries->type() != JsonType::Array)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           featureContext +
                                               ".edgeSelection: expected an array");
                    EdgeSelection selection;
                    for (std::size_t q = 0; q < queries->items().size(); ++q) {
                        const std::string queryContext =
                            featureContext + ".edgeSelection[" + std::to_string(q) + "]";
                        const JsonValue& entry = queries->items()[q];
                        if (entry.type() != JsonType::Object)
                            return loadFailure(SerializationError::InvalidFieldType,
                                               queryContext + ": entry is not an object");
                        EdgeQuery query;
                        if (!ReadEdgeQuery(entry, queryContext, err, query))
                            return loadFailure(err.error, err.message);
                        selection.push_back(query);
                    }
                    // An EMPTY array is refused rather than read as "every
                    // edge": they are opposite solids, and a file that says
                    // "nothing" must not come back saying "everything".
                    if (selection.empty())
                        return loadFailure(SerializationError::InvalidFieldType,
                                           featureContext +
                                               ".edgeSelection: an empty selection names no "
                                               "edge and is not the same as every edge");
                    featureData.edgeSelection = std::move(selection);
                }
                featureData.materialId = *dressMaterialId;
            }

            body.features.push_back(std::move(featureData));
        }
        bodyData.push_back(std::move(body));
    }

    // Sketches (v4, ADR-M4-001/002). Parsed and fully validated BEFORE any
    // document construction, like every other section, so a malformed file can
    // never leave a partial document nor advance the id generator.
    struct SketchEntityData {
        SketchEntityId id{kInvalidSketchEntityId};
        SketchGeometry geometry{};
        bool construction{false};
    };
    struct SketchConstraintData_ {
        SketchConstraintId id{kInvalidSketchConstraintId};
        SketchConstraintData data{};
        bool driven = false; // v20, optional
    };
    struct SketchPlacementData {
        SketchConstraintId constraintId{kInvalidSketchConstraintId};
        Vec2 labelMm{};
    };
    struct SketchFormatData {
        SketchConstraintId constraintId{kInvalidSketchConstraintId};
        std::string prefix;
        std::string suffix;
        double plusTolerance{0.0};
        double minusTolerance{0.0};
    };
    struct SketchData {
        ObjectId id;
        std::string name;
        Transform3D transform;
        std::vector<SketchEntityData> entities;
        std::vector<SketchConstraintData_> constraints;
        ObjectId supportFrameId = kInvalidObjectId; // v10, optional
        std::optional<FaceQuery> trackedFace;       // v19, optional
        std::vector<SketchPlacementData> placements; // v12, optional
        std::vector<SketchFormatData> formats;       // v12, optional
        std::vector<SketchReference> references;      // v17, optional
    };
    std::vector<SketchData> sketchData;
    std::unordered_set<ObjectId> sketchIds;

    const JsonValue* sketchesField = root.find("sketches");
    if (sketchesField != nullptr) {
        if (sketchesField->type() != JsonType::Array)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'sketches' has the wrong JSON type");
        for (std::size_t i = 0; i < sketchesField->items().size(); ++i) {
            const std::string context = "sketches[" + std::to_string(i) + "]";
            const JsonValue& entry = sketchesField->items()[i];
            if (entry.type() != JsonType::Object)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": entry is not an object");
            const auto sketchId = requireIdField(entry, context, err);
            if (!sketchId) return loadFailure(err.error, err.message);
            if (!registerId(*sketchId, context, err)) return loadFailure(err.error, err.message);
            sketchIds.insert(*sketchId);

            const JsonValue* nameField =
                requireField(entry, "name", JsonType::String, context, err);
            if (nameField == nullptr) return loadFailure(err.error, err.message);

            SketchData data{*sketchId, nameField->asString(), Transform3D{}, {}};

            const JsonValue* frameField =
                requireField(entry, "frame", JsonType::Object, context, err);
            if (frameField == nullptr) return loadFailure(err.error, err.message);
            const JsonValue* translationField =
                requireField(*frameField, "translation", JsonType::Object, context, err);
            if (translationField == nullptr) return loadFailure(err.error, err.message);
            const JsonValue* rotationField =
                requireField(*frameField, "rotation", JsonType::Object, context, err);
            if (rotationField == nullptr) return loadFailure(err.error, err.message);
            const auto number = [&](const JsonValue& object, const char* key,
                                    double& out) -> bool {
                const JsonValue* field = requireField(object, key, JsonType::Number, context, err);
                if (field == nullptr) return false;
                out = field->asNumber();
                return true;
            };
            if (!number(*translationField, "x", data.transform.translation.x) ||
                !number(*translationField, "y", data.transform.translation.y) ||
                !number(*translationField, "z", data.transform.translation.z) ||
                !number(*rotationField, "w", data.transform.rotation.w) ||
                !number(*rotationField, "x", data.transform.rotation.x) ||
                !number(*rotationField, "y", data.transform.rotation.y) ||
                !number(*rotationField, "z", data.transform.rotation.z))
                return loadFailure(err.error, err.message);

            const JsonValue* entitiesField =
                requireField(entry, "entities", JsonType::Array, context, err);
            if (entitiesField == nullptr) return loadFailure(err.error, err.message);
            std::unordered_set<ObjectId> entityIdsInSketch;
            for (std::size_t j = 0; j < entitiesField->items().size(); ++j) {
                const std::string entityContext = context + ".entities[" + std::to_string(j) + "]";
                const JsonValue& entityEntry = entitiesField->items()[j];
                if (entityEntry.type() != JsonType::Object)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       entityContext + ": entry is not an object");
                const auto entityId = requireIdField(entityEntry, entityContext, err);
                if (!entityId) return loadFailure(err.error, err.message);
                if (*entityId > kMaxObjectId)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       entityContext + ": entity id exceeds the id cap");
                // Entity ids share the ObjectId space but are scoped to their
                // sketch, so uniqueness is enforced WITHIN the sketch rather
                // than against the document-wide id set.
                if (!entityIdsInSketch.insert(*entityId).second)
                    return loadFailure(SerializationError::DuplicateId,
                                       entityContext + ": duplicate sketch entity id " +
                                           idToString(*entityId));

                SketchEntityData entity;
                entity.id = static_cast<SketchEntityId>(*entityId);
                if (!ReadSketchGeometry(entityEntry, entityContext, err, entity.geometry))
                    return loadFailure(err.error, err.message);
                // v14, and OPTIONAL: absent means false, which is what every
                // file written before v14 says by omission. A wrong TYPE is
                // still refused -- silently treating a string as false would
                // turn a corrupt file into a quietly different model.
                if (const JsonValue* construction = entityEntry.find("construction")) {
                    if (construction->type() != JsonType::Bool)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           entityContext + ".construction: expected a boolean");
                    entity.construction = construction->asBool();
                }
                data.entities.push_back(std::move(entity));
            }

            // References (v17). OPTIONAL, exactly as constraints are: every
            // file written before v17 simply has no such array, and a sketch
            // with no underlay is what it always was.
            const JsonValue* referencesField = entry.find("references");
            if (referencesField != nullptr) {
                if (referencesField->type() != JsonType::Array)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'references' has the wrong JSON type");
                std::unordered_set<ObjectId> referenceIdsInSketch;
                for (std::size_t j = 0; j < referencesField->items().size(); ++j) {
                    const std::string rc = context + ".references[" + std::to_string(j) + "]";
                    const JsonValue& re = referencesField->items()[j];
                    if (re.type() != JsonType::Object)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           rc + ": entry is not an object");
                    const auto referenceId = requireIdField(re, rc, err);
                    if (!referenceId) return loadFailure(err.error, err.message);
                    if (*referenceId > kMaxObjectId)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           rc + ": reference id exceeds the id cap");
                    // Scoped to the sketch, exactly like entity and constraint
                    // ids. A duplicate is refused rather than merged: two
                    // references with one id is a file whose author's intent
                    // cannot be recovered.
                    if (!referenceIdsInSketch.insert(*referenceId).second)
                        return loadFailure(SerializationError::DuplicateId,
                                           rc + ": duplicate sketch reference id " +
                                               idToString(*referenceId));
                    SketchReference reference;
                    reference.id = static_cast<SketchReferenceId>(*referenceId);
                    if (!ReadSketchGeometry(re, rc, err, reference.geometry))
                        return loadFailure(err.error, err.message);
                    data.references.push_back(std::move(reference));
                }
            }

            // Constraints (v5). OPTIONAL: a v4 file has no such array and must
            // still load, as a sketch with free geometry and no constraints --
            // which is exactly what it was.
            const JsonValue* constraintsField = entry.find("constraints");
            if (constraintsField != nullptr) {
                if (constraintsField->type() != JsonType::Array)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'constraints' has the wrong JSON type");
                std::unordered_set<ObjectId> constraintIdsInSketch;
                for (std::size_t j = 0; j < constraintsField->items().size(); ++j) {
                    const std::string cc = context + ".constraints[" + std::to_string(j) + "]";
                    const JsonValue& ce = constraintsField->items()[j];
                    if (ce.type() != JsonType::Object)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           cc + ": entry is not an object");

                    const auto constraintId = requireIdField(ce, cc, err);
                    if (!constraintId) return loadFailure(err.error, err.message);
                    if (*constraintId > kMaxObjectId)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           cc + ": constraint id exceeds the id cap");
                    // Scoped to the sketch, exactly like entity ids.
                    if (!constraintIdsInSketch.insert(*constraintId).second)
                        return loadFailure(SerializationError::DuplicateId,
                                           cc + ": duplicate sketch constraint id " +
                                               idToString(*constraintId));

                    const JsonValue* kindField =
                        requireField(ce, "type", JsonType::String, cc, err);
                    if (kindField == nullptr) return loadFailure(err.error, err.message);
                    const std::string kind = kindField->asString();

                    // Every reference is resolved against THIS sketch's entity
                    // set as parsed above -- a constraint naming an entity that
                    // is not in the file is rejected at load, never restored as
                    // a dangling reference (spec 17).
                    bool refError = false;
                    std::string refMessage;
                    SerializationError refCode = SerializationError::None;
                    const auto entityRef = [&](const char* key) -> SketchEntityId {
                        if (refError) return kInvalidSketchEntityId;
                        const JsonValue* field =
                            requireField(ce, key, JsonType::String, cc, err);
                        if (field == nullptr) {
                            refError = true;
                            refCode = err.error;
                            refMessage = err.message;
                            return kInvalidSketchEntityId;
                        }
                        const auto id = idFromString(field->asString());
                        if (!id || *id == kInvalidObjectId || *id > kMaxObjectId) {
                            refError = true;
                            refCode = SerializationError::InvalidFieldType;
                            refMessage = cc + ": field '" + key + "' is not a valid id";
                            return kInvalidSketchEntityId;
                        }
                        if (entityIdsInSketch.count(*id) == 0) {
                            refError = true;
                            refCode = SerializationError::UnknownDependencyId;
                            refMessage = cc + ": references entity " + idToString(*id) +
                                         " which is not in this sketch";
                            return kInvalidSketchEntityId;
                        }
                        return static_cast<SketchEntityId>(*id);
                    };
                    const auto elementRef = [&](const char* key) -> SketchElementRef {
                        if (refError) return SketchElementRef{};
                        const JsonValue* field =
                            requireField(ce, key, JsonType::Object, cc, err);
                        if (field == nullptr) {
                            refError = true;
                            refCode = err.error;
                            refMessage = err.message;
                            return SketchElementRef{};
                        }
                        const JsonValue* entityField = requireField(*field, "entityId",
                                                                    JsonType::String, cc, err);
                        const JsonValue* subField = requireField(*field, "subElement",
                                                                 JsonType::String, cc, err);
                        if (entityField == nullptr || subField == nullptr) {
                            refError = true;
                            refCode = err.error;
                            refMessage = err.message;
                            return SketchElementRef{};
                        }
                        const auto id = idFromString(entityField->asString());
                        if (!id || *id == kInvalidObjectId || *id > kMaxObjectId ||
                            entityIdsInSketch.count(*id) == 0) {
                            refError = true;
                            refCode = SerializationError::UnknownDependencyId;
                            refMessage = cc + ": field '" + key +
                                         "' references an entity that is not in this sketch";
                            return SketchElementRef{};
                        }
                        const auto sub = subElementFromString(subField->asString());
                        if (!sub) {
                            refError = true;
                            refCode = SerializationError::InvalidEnumValue;
                            refMessage = cc + ": unknown sub-element '" + subField->asString() +
                                         "'";
                            return SketchElementRef{};
                        }
                        // The INDEX, and only where it means something. Absent
                        // is 0, which is what every file written before M17.30
                        // meant -- but present-and-broken is an error, because
                        // a spline point that quietly became point 0 would be a
                        // constraint on a different point than the one saved.
                        int index = 0;
                        if (const JsonValue* indexField = field->find("index")) {
                            if (indexField->type() != JsonType::Number) {
                                refError = true;
                                refCode = SerializationError::InvalidFieldType;
                                refMessage = cc + ": field '" + key + ".index' is not a number";
                                return SketchElementRef{};
                            }
                            const double raw = indexField->asNumber();
                            if (raw < 0.0 || raw != std::floor(raw) || raw > 1e6) {
                                refError = true;
                                refCode = SerializationError::InvalidFieldType;
                                refMessage = cc + ": field '" + key +
                                             ".index' is not a whole point number";
                                return SketchElementRef{};
                            }
                            index = static_cast<int>(raw);
                        }
                        return SketchElementRef{static_cast<SketchEntityId>(*id), *sub, index};
                    };
                    // The bound Parameter must exist in this file. parameterIds
                    // was built before any sketch was parsed.
                    const auto parameterRef = [&]() -> ObjectId {
                        if (refError) return kInvalidObjectId;
                        const JsonValue* field =
                            requireField(ce, "parameterId", JsonType::String, cc, err);
                        if (field == nullptr) {
                            refError = true;
                            refCode = err.error;
                            refMessage = err.message;
                            return kInvalidObjectId;
                        }
                        const auto id = idFromString(field->asString());
                        if (!id || *id == kInvalidObjectId || *id > kMaxObjectId) {
                            refError = true;
                            refCode = SerializationError::InvalidFieldType;
                            refMessage = cc + ": field 'parameterId' is not a valid id";
                            return kInvalidObjectId;
                        }
                        if (parameterIds.count(*id) == 0) {
                            refError = true;
                            refCode = SerializationError::UnknownDependencyId;
                            refMessage = cc + ": parameter id " + idToString(*id) +
                                         " is not a parameter in this document";
                            return kInvalidObjectId;
                        }
                        return *id;
                    };

                    SketchConstraintData_ parsed;
                    parsed.id = static_cast<SketchConstraintId>(*constraintId);
                    if (kind == "Coincident") {
                        CoincidentConstraint c;
                        c.a = elementRef("a");
                        c.b = elementRef("b");
                        parsed.data = c;
                    } else if (kind == "Horizontal") {
                        parsed.data = HorizontalConstraint{entityRef("line")};
                    } else if (kind == "Vertical") {
                        parsed.data = VerticalConstraint{entityRef("line")};
                    } else if (kind == "PointsHorizontal") {
                        PointsHorizontalConstraint c;
                        c.a = elementRef("a");
                        c.b = elementRef("b");
                        parsed.data = c;
                    } else if (kind == "PointsVertical") {
                        PointsVerticalConstraint c;
                        c.a = elementRef("a");
                        c.b = elementRef("b");
                        parsed.data = c;
                    } else if (kind == "Fix") {
                        parsed.data = FixConstraint{elementRef("target")};
                    } else if (kind == "Distance") {
                        DistanceConstraint c;
                        c.a = elementRef("a");
                        c.b = elementRef("b");
                        parsed.data = c;
                    } else if (kind == "HorizontalDistance") {
                        HorizontalDistanceConstraint c;
                        c.a = elementRef("a");
                        c.b = elementRef("b");
                        parsed.data = c;
                    } else if (kind == "VerticalDistance") {
                        VerticalDistanceConstraint c;
                        c.a = elementRef("a");
                        c.b = elementRef("b");
                        parsed.data = c;
                    } else if (kind == "Symmetric") {
                        SymmetricConstraint c;
                        c.a = elementRef("a");
                        c.b = elementRef("b");
                        c.line = entityRef("line");
                        parsed.data = c;
                    } else if (kind == "PointLineDistance") {
                        PointLineDistanceConstraint c;
                        c.point = elementRef("point");
                        c.line = entityRef("line");
                        parsed.data = c;
                    } else if (kind == "Length") {
                        LengthConstraint c;
                        c.line = entityRef("line");
                        parsed.data = c;
                    } else if (kind == "Radius") {
                        RadiusConstraint c;
                        c.curve = entityRef("curve");
                        parsed.data = c;
                    } else if (kind == "Diameter") {
                        DiameterConstraint c;
                        c.curve = entityRef("curve");
                        parsed.data = c;
                    } else if (kind == "Angle") {
                        AngleConstraint c;
                        c.lineA = entityRef("lineA");
                        c.lineB = entityRef("lineB");
                        parsed.data = c;
                    } else if (kind == "Parallel") {
                        ParallelConstraint c;
                        c.lineA = entityRef("lineA");
                        c.lineB = entityRef("lineB");
                        parsed.data = c;
                    } else if (kind == "Perpendicular") {
                        PerpendicularConstraint c;
                        c.lineA = entityRef("lineA");
                        c.lineB = entityRef("lineB");
                        parsed.data = c;
                    } else if (kind == "Equal") {
                        EqualConstraint c;
                        c.a = entityRef("a");
                        c.b = entityRef("b");
                        parsed.data = c;
                    } else if (kind == "Concentric") {
                        ConcentricConstraint c;
                        c.curveA = entityRef("curveA");
                        c.curveB = entityRef("curveB");
                        parsed.data = c;
                    } else if (kind == "Midpoint") {
                        MidpointConstraint c;
                        c.point = elementRef("point");
                        c.line = entityRef("line");
                        parsed.data = c;
                    } else if (kind == "PointOnObject") {
                        PointOnObjectConstraint c;
                        c.point = elementRef("point");
                        c.target = entityRef("target");
                        parsed.data = c;
                    } else if (kind == "EllipseRotation") {
                        EllipseRotationConstraint c;
                        c.curve = entityRef("curve");
                        parsed.data = c;
                    } else if (kind == "MajorAxis" || kind == "MinorAxis") {
                        EllipseAxisConstraint c;
                        c.curve = entityRef("curve");
                        // REQUIRED, like Tangent's `internal` and for the same
                        // reason: defaulting it would turn a truncated file
                        // into a valid document dimensioning the OTHER axis.
                        const JsonValue* minor =
                            requireField(ce, "minor", JsonType::Bool, cc, err);
                        if (minor == nullptr) return loadFailure(err.error, err.message);
                        c.minor = minor->asBool();
                        parsed.data = c;
                    } else if (kind == "Tangent") {
                        TangentConstraint c;
                        c.a = entityRef("a");
                        c.b = entityRef("b");
                        // REQUIRED, not defaulted. Defaulting it would turn a
                        // truncated file into a valid document describing the
                        // OTHER tangency, silently.
                        const JsonValue* internal =
                            requireField(ce, "internal", JsonType::Bool, cc, err);
                        if (internal == nullptr) return loadFailure(err.error, err.message);
                        c.internal = internal->asBool();
                        // v21, OPTIONAL: absent means a line free to slide,
                        // which is what every file before v21 meant. Present
                        // and unreadable is still an error -- a tangency that
                        // quietly forgot WHERE it holds is the rank-deficient
                        // case again, and it would look fine until the sketch
                        // kinked.
                        if (const JsonValue* at = ce.find("at")) {
                            if (at->type() != JsonType::String)
                                return loadFailure(SerializationError::InvalidFieldType,
                                                   cc + ".at: expected a string");
                            const auto part = subElementFromString(at->asString());
                            if (!part.has_value())
                                return loadFailure(SerializationError::InvalidEnumValue,
                                                   cc + ".at: unknown sub-element '" +
                                                       at->asString() + "'");
                            c.at = *part;
                        }
                        parsed.data = c;
                    } else {
                        return loadFailure(SerializationError::InvalidEnumValue,
                                           cc + ": unknown sketch constraint type '" + kind + "'");
                    }
                    // THE BOUND PARAMETER, for every dimensional kind, HERE.
                    //
                    // It used to be a line inside each kind's own branch --
                    // eight copies of the same statement, and a ninth and tenth
                    // that were never written when M17.25 added the ellipse's
                    // axis and orientation dimensions. Those two constraints
                    // saved perfectly and came back bound to nothing, so a file
                    // that solved before it was written refused to solve after
                    // it was read, naming a constraint id and no cause.
                    //
                    // The WRITER was already generic (BoundParameterId). This is
                    // the same list read the other way, so the two cannot
                    // disagree about which kinds have one.
                    if (IsDimensional(parsed.data)) {
                        const ObjectId bound = parameterRef();
                        VisitBoundParameter(parsed.data,
                                            [bound](ObjectId& id) { id = bound; });
                    }

                    if (refError) return loadFailure(refCode, refMessage);
                    // v20, OPTIONAL: absent means DRIVING, which is what every file
                    // written before this said by omission. A wrong TYPE is refused --
                    // reading a string as false would turn a corrupt file into a sketch
                    // with one more constraint than its author drew.
                    if (const JsonValue* driven = ce.find("driven")) {
                        if (driven->type() != JsonType::Bool)
                            return loadFailure(SerializationError::InvalidFieldType,
                                               cc + ".driven: expected a boolean");
                        parsed.driven = driven->asBool();
                    }
                    data.constraints.push_back(std::move(parsed));
                }
            }

            // v12: where the user dragged each dimension's value.
            //
            // OPTIONAL, and absent in every file written before v12 -- those
            // documents simply place every dimension automatically, which is
            // exactly what they did when they were written. A missing array is
            // therefore not an error; a malformed one is.
            const JsonValue* placementsField = entry.find("dimensionPlacements");
            if (placementsField != nullptr) {
                if (placementsField->type() != JsonType::Array)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context +
                                           ": field 'dimensionPlacements' has the wrong JSON "
                                           "type");
                for (std::size_t pi = 0; pi < placementsField->items().size(); ++pi) {
                    const JsonValue& pv = placementsField->items()[pi];
                    const std::string pc =
                        context + ": dimensionPlacement " + std::to_string(pi);
                    if (pv.type() != JsonType::Object)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           pc + " is not an object");
                    const JsonValue* idField =
                        requireField(pv, "constraintId", JsonType::String, pc, err);
                    const JsonValue* uField = requireField(pv, "u", JsonType::Number, pc, err);
                    const JsonValue* vField = requireField(pv, "v", JsonType::Number, pc, err);
                    if (idField == nullptr || uField == nullptr || vField == nullptr)
                        return loadFailure(err.error, err.message);
                    const auto placedId = idFromString(idField->asString());
                    if (!placedId || *placedId == kInvalidObjectId || *placedId > kMaxObjectId)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           pc + ": 'constraintId' is not a valid id");
                    if (!std::isfinite(uField->asNumber()) || !std::isfinite(vField->asNumber()))
                        return loadFailure(SerializationError::InvalidFieldType,
                                           pc + ": position is not a finite point");
                    SketchPlacementData placement;
                    placement.constraintId = static_cast<SketchConstraintId>(*placedId);
                    placement.labelMm = Vec2{uField->asNumber(), vField->asNumber()};
                    data.placements.push_back(placement);
                }
            }

            // v12: how each dimension's value reads. Optional, for the same
            // reason the placements array is.
            const JsonValue* formatsField = entry.find("dimensionFormats");
            if (formatsField != nullptr) {
                if (formatsField->type() != JsonType::Array)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context +
                                           ": field 'dimensionFormats' has the wrong JSON type");
                for (std::size_t fi = 0; fi < formatsField->items().size(); ++fi) {
                    const JsonValue& fv = formatsField->items()[fi];
                    const std::string fc =
                        context + ": dimensionFormat " + std::to_string(fi);
                    if (fv.type() != JsonType::Object)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           fc + " is not an object");
                    const JsonValue* idField =
                        requireField(fv, "constraintId", JsonType::String, fc, err);
                    const JsonValue* prefixField =
                        requireField(fv, "prefix", JsonType::String, fc, err);
                    const JsonValue* suffixField =
                        requireField(fv, "suffix", JsonType::String, fc, err);
                    const JsonValue* plusField =
                        requireField(fv, "plusTolerance", JsonType::Number, fc, err);
                    const JsonValue* minusField =
                        requireField(fv, "minusTolerance", JsonType::Number, fc, err);
                    if (idField == nullptr || prefixField == nullptr || suffixField == nullptr ||
                        plusField == nullptr || minusField == nullptr)
                        return loadFailure(err.error, err.message);
                    const auto formattedId = idFromString(idField->asString());
                    if (!formattedId || *formattedId == kInvalidObjectId ||
                        *formattedId > kMaxObjectId)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           fc + ": 'constraintId' is not a valid id");
                    if (!std::isfinite(plusField->asNumber()) ||
                        !std::isfinite(minusField->asNumber()))
                        return loadFailure(SerializationError::InvalidFieldType,
                                           fc + ": a tolerance is not a finite number");
                    SketchFormatData format;
                    format.constraintId = static_cast<SketchConstraintId>(*formattedId);
                    format.prefix = prefixField->asString();
                    format.suffix = suffixField->asString();
                    format.plusTolerance = plusField->asNumber();
                    format.minusTolerance = minusField->asNumber();
                    data.formats.push_back(std::move(format));
                }
            }

            // v10, OPTIONAL: absent means the sketch uses its own embedded
            // plane, which is every pre-M10 file.
            if (const JsonValue* support = entry.find("supportFrameId")) {
                if (support->type() != JsonType::String)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'supportFrameId' is not a string");
                const auto parsed = idFromString(support->asString());
                if (!parsed.has_value() || *parsed == kInvalidObjectId || *parsed > kMaxObjectId)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context +
                                           ": field 'supportFrameId' is not a valid ObjectId");
                data.supportFrameId = *parsed;
            }

            // v19, OPTIONAL: the face this sketch follows (M17.14). Absent
            // means the embedded frame is the whole story, which is every file
            // written before this and every sketch on a world plane.
            if (const JsonValue* tracked = entry.find("trackedFace")) {
                if (tracked->type() != JsonType::Object)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'trackedFace' is not an object");
                FaceQuery query;
                if (!ReadFaceQuery(*tracked, context + ".trackedFace", err, query))
                    return loadFailure(err.error, err.message);
                data.trackedFace = query;
            }
            sketchData.push_back(std::move(data));
        }
    }

    // Every Pad must reference a Sketch present in this file. Same rule as the
    // Box parameter check above: a reference the loader would reject must never
    // have been savable either (ADR-M3-008).
    for (const auto& body : bodyData) {
        for (const auto& feature : body.features) {
            if (feature.typeName != "Pad") continue;
            if (sketchIds.count(feature.sketchId) == 0)
                return loadFailure(SerializationError::UnknownDependencyId,
                                   "feature " + idToString(feature.id) + ": pad sketch id " +
                                       idToString(feature.sketchId) +
                                       " is not a sketch in this document");
        }
    }

    // Every consumer (Pocket/Fillet/Chamfer) must reference a base that is an
    // earlier SOLID feature of the same body, and no base may be consumed
    // twice -- restore replays features in array order, so a base that follows
    // its consumer (or lives in another body) would be an edge to a node that
    // does not exist yet, and a base that is not a solid type (round 1's
    // R2-M2: a Placeholder) is a node that will NEVER exist, its failed edge
    // silently discarded. A doubly-consumed base is round 1's R1-C1 diamond.
    // validateSaveable enforces the same rules at save time; the solid-type
    // decision comes from the ONE shared table (kSolidFeatureTypeNames, top of
    // file) so the two doors cannot drift apart silently.
    for (const auto& body : bodyData) {
        std::unordered_set<ObjectId> earlierSolids;
        std::unordered_set<ObjectId> consumedBases;
        for (const auto& feature : body.features) {
            const bool consumes = IsConsumingFeatureTypeName(feature.typeName);
            if (feature.typeName == "Pocket" && sketchIds.count(feature.sketchId) == 0)
                return loadFailure(SerializationError::UnknownDependencyId,
                                   "feature " + idToString(feature.id) +
                                       ": pocket sketch id " + idToString(feature.sketchId) +
                                       " is not a sketch in this document");
            if (consumes) {
                const std::string noun =
                    feature.typeName == "Pocket" ? "pocket" : "fillet/chamfer";
                if (earlierSolids.count(feature.baseFeatureId) == 0)
                    return loadFailure(SerializationError::UnknownDependencyId,
                                       "feature " + idToString(feature.id) + ": " + noun +
                                           " base feature id " +
                                           idToString(feature.baseFeatureId) +
                                           " is not an earlier solid feature of the same body");
                if (!consumedBases.insert(feature.baseFeatureId).second)
                    return loadFailure(SerializationError::UnknownDependencyId,
                                       "feature " + idToString(feature.id) + ": " + noun +
                                           " base feature id " +
                                           idToString(feature.baseFeatureId) +
                                           " is already consumed by an earlier feature; a "
                                           "solid may be consumed once (ADR-M8-008)");
            }
            if (IsSolidFeatureTypeName(feature.typeName)) earlierSolids.insert(feature.id);
        }
    }

    // Every Revolve must reference a Sketch in this file, and its axis must be
    // an entity OF that sketch -- an axis id that resolves to nothing, or to an
    // entity of a different sketch, is a file the saver could never have
    // written (ADR-M3-008).
    for (const auto& body : bodyData) {
        for (const auto& feature : body.features) {
            if (feature.typeName != "Revolve") continue;
            const auto sketchIt =
                std::find_if(sketchData.begin(), sketchData.end(),
                             [&](const auto& sk) { return sk.id == feature.sketchId; });
            if (sketchIt == sketchData.end())
                return loadFailure(SerializationError::UnknownDependencyId,
                                   "feature " + idToString(feature.id) +
                                       ": revolve sketch id " + idToString(feature.sketchId) +
                                       " is not a sketch in this document");
            const bool axisInSketch =
                std::any_of(sketchIt->entities.begin(), sketchIt->entities.end(),
                            [&](const auto& entity) {
                                return ToObjectId(entity.id) == feature.axisEntityId;
                            });
            if (!axisInSketch)
                return loadFailure(SerializationError::UnknownDependencyId,
                                   "feature " + idToString(feature.id) +
                                       ": revolve axis entity id " +
                                       idToString(feature.axisEntityId) +
                                       " is not an entity of sketch " +
                                       idToString(feature.sketchId));
        }
    }

    // Dependencies (Option A, ADR-012/ADR-M3-005; optional array). Validated
    // fully BEFORE any document construction on a scratch graph holding
    // exactly the node set the real document will have (parameter nodes --
    // Feature/MassPropertiesNode edges are Option B and never appear here),
    // so a bad edge can never leave a partial document nor advance the id
    // generator. parameterIds was already built above (also used to validate
    // BoxFeature references).
    struct EdgeData {
        ObjectId prerequisite;
        ObjectId dependent;
    };
    std::vector<EdgeData> edgeData;
    const JsonValue* dependencies = root.find("dependencies");
    if (dependencies != nullptr) {
        if (dependencies->type() != JsonType::Array)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'dependencies' has the wrong JSON type");
        DependencyGraph scratch;
        for (ObjectId id : parameterIds) scratch.addNode(id);

        const auto endpoint = [&](const JsonValue& entry, const char* key,
                                  const std::string& context,
                                  FieldError& fieldErr) -> std::optional<ObjectId> {
            const JsonValue* field = requireField(entry, key, JsonType::String, context, fieldErr);
            if (field == nullptr) return std::nullopt;
            const auto id = idFromString(field->asString());
            if (!id.has_value() || *id == kInvalidObjectId || *id > kMaxObjectId) {
                fieldErr = fieldError(SerializationError::InvalidFieldType,
                                      context + ": field '" + key +
                                          "' is not a valid decimal ObjectId string");
                return std::nullopt;
            }
            return id;
        };

        for (std::size_t i = 0; i < dependencies->items().size(); ++i) {
            const JsonValue& entry = dependencies->items()[i];
            const std::string context = "dependencies[" + std::to_string(i) + "]";
            if (entry.type() != JsonType::Object)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": entry is not an object");
            const auto prerequisite = endpoint(entry, "prerequisite", context, err);
            if (!prerequisite.has_value()) return loadFailure(err.error, err.message);
            const auto dependent = endpoint(entry, "dependent", context, err);
            if (!dependent.has_value()) return loadFailure(err.error, err.message);

            for (ObjectId endpointId : {*prerequisite, *dependent}) {
                if (parameterIds.count(endpointId) != 0) continue;
                const bool persisted = seenIds.count(endpointId) != 0;
                return loadFailure(SerializationError::UnknownDependencyId,
                                   context + ": id " + idToString(endpointId) +
                                       (persisted ? " is not a dependency-graph node"
                                                  : " is not an object in this document"));
            }
            const GraphResult applied = scratch.addDependency(*dependent, *prerequisite);
            if (!applied)
                return loadFailure(SerializationError::InvalidDependency,
                                   context + ": edge " + idToString(*prerequisite) + " -> " +
                                       idToString(*dependent) +
                                       " is a self-edge or would create a cycle");
            edgeData.push_back(EdgeData{*prerequisite, *dependent});
        }
    }

    // Construction. Advance the generator past the file's MAXIMUM persisted id
    // first: restore allocates fresh ids along the way (massPropertiesNode_ and
    // the Origin frame, both built by the PartDocument constructor BEFORE any
    // restore runs), and those must not collide with persisted ids restored
    // later. The per-restore RestoreObjectId calls remain as defense in depth.
    //
    // Scope of the never-advance-on-failure guarantee: it holds for every
    // VALIDATION failure, all of which return before this line. It does NOT
    // hold for the two failures that can still occur after it -- a restore that
    // throws, and the defensive edge re-apply -- where the generator has
    // already moved. Both leave no document, and ids only ever move forward, so
    // nothing is corrupted; but the guarantee is not unconditional and saying
    // so here is cheaper than a reader assuming it is.
    ObjectId maxPersistedId = *documentId;
    for (const auto& parameter : parameterData)
        maxPersistedId = std::max(maxPersistedId, parameter.id);
    if (materialData.has_value()) maxPersistedId = std::max(maxPersistedId, materialData->id);
    for (const auto& body : bodyData) {
        maxPersistedId = std::max(maxPersistedId, body.id);
        for (const auto& feature : body.features)
            maxPersistedId = std::max(maxPersistedId, feature.id);
    }
    // Sketch, ENTITY and CONSTRAINT ids too. All three are written to the file
    // and all three come from the same ObjectIdGenerator, so omitting them
    // leaves the counter below the file's real maximum -- and in a v5 file the
    // entity and constraint ids are typically the LARGEST ids present.
    //
    // The consequence was silent and severe: PartDocument's constructor
    // allocates massPropertiesNode_ and the Origin frame from the
    // under-advanced counter, so one of them takes an id a sketch in this very
    // file already owns. restoreSketch's registerObject then fails (duplicate
    // id) while graph_.addNode succeeds, and the two objects share a node: the
    // Pad loses its profile AND the sketch is never invoked, with no error
    // anywhere and a re-save that still loads.
    //
    // Every serialization test saves and loads in ONE process, where the
    // generator is already past every id -- which is why the whole suite was
    // structurally blind to this. M5_SER_015 loads from a cold generator.
    for (const auto& sketch : sketchData) {
        maxPersistedId = std::max(maxPersistedId, sketch.id);
        for (const auto& entity : sketch.entities)
            maxPersistedId = std::max(maxPersistedId, ToObjectId(entity.id));
        for (const auto& constraint : sketch.constraints)
            maxPersistedId = std::max(maxPersistedId, ToObjectId(constraint.id));
    }
    ObjectIdGenerator::AdvancePast(maxPersistedId);

    // All restore goes through the document facade so ObjectRegistry and
    // DependencyGraph are rebuilt during load (M2-SER-003). Graph states are
    // not persisted: every restored node starts Dirty. Order matters: Material
    // before bodies/features, since a BoxFeature's Material->MassPropertiesNode
    // edge (Option B, ADR-M3-005) requires the Material to already be a graph
    // node when restoreBoxFeature wires it.
    // Restore runs inside a try: PartDocument's restore paths throw on an
    // invariant violation (an id that collides with an object the constructor
    // already allocated). That must surface as a load failure, not as an
    // exception escaping the serializer -- and it must never be swallowed into
    // a half-built document, which is what returning a partial result would do.
    try {
    auto document = std::make_unique<PartDocument>(*documentId, name->asString());
    for (auto& parameter : parameterData)
        document->restoreParameter(parameter.id, std::move(parameter.name), parameter.value,
                                   parameter.unit, std::move(parameter.expression),
                                   parameter.state);
    // Expression edges are NOT persisted -- the graph is rebuilt on load -- so
    // they are re-derived here, ONCE, after every parameter exists. Doing it
    // inside restoreParameter would refuse every forward reference, since the
    // file lists parameters in document order and an expression may legally
    // read one written later.
    //
    // A failure means the file is not loadable as written: refused, rather than
    // opening a document whose parameters would evaluate in the wrong order or
    // not at all. This records no undo step (ADR-M9-001).
    if (const PartDocument::ExpressionWiringResult wiring =
            document->rewireParameterExpressions();
        !wiring.ok) {
        return loadFailure(SerializationError::InvalidDependency, wiring.message);
    }
    // v51 (M51). SHEET METAL, read as one setting.
    if (const JsonValue* sheet = root.find("sheetMetal")) {
        if (sheet->type() != JsonType::Object)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'sheetMetal' is not an object");
        SheetMetalSettings settings;
        settings.isSheetMetal = true;
        const JsonValue* thickness =
            requireField(*sheet, "thicknessMm", JsonType::Number, "sheetMetal", err);
        if (thickness == nullptr) return loadFailure(err.error, err.message);
        settings.thicknessMm = thickness->asNumber();
        const JsonValue* material =
            requireField(*sheet, "material", JsonType::String, "sheetMetal", err);
        if (material == nullptr) return loadFailure(err.error, err.message);
        // REFUSED, NOT DEFAULTED. A class this build does not know would
        // become mild steel, whose K is a tenth from spring steel's -- and a
        // tenth of a K is millimetres across an enclosure.
        if (!ParseSheetMaterial(material->asString(), settings.material))
            return loadFailure(SerializationError::InvalidEnumValue,
                               "sheetMetal: unknown material '" + material->asString() + "'");
        const JsonValue* radius =
            requireField(*sheet, "defaultBendRadiusMm", JsonType::Number, "sheetMetal", err);
        if (radius == nullptr) return loadFailure(err.error, err.message);
        settings.defaultBendRadiusMm = radius->asNumber();
        // WHAT THE SAVER REFUSES, THE LOADER REFUSES, by calling the same
        // function (ADR-M3-008) -- a default radius the material cracks at
        // would otherwise open as a part every one of whose bends is refused,
        // one at a time, after it is drawn.
        const std::string why = document->whySheetMetalRefused(settings);
        if (!why.empty())
            return loadFailure(SerializationError::InvalidFieldType, "sheetMetal: " + why);
        // Straight onto the document, not through the undoable setter: loading
        // is not an edit, and a freshly opened part with one step of history
        // is a part somebody can undo into a state the file never held.
        document->restoreSheetMetal(settings);
    }
    if (materialData.has_value()) {
        document->restoreMaterial(materialData->id, std::move(materialData->name),
                                  materialData->densityKgPerM3, materialData->elasticModulusPa,
                                  materialData->poissonRatio, materialData->yieldStrengthPa,
                                  materialData->contact);
    }
    // Frames BEFORE sketches: a sketch's support reference is a graph edge to
    // its frame, so the frame has to be a node already. Parents before
    // children within the frame list is guaranteed by writing them in document
    // order, and restoreFrame refuses a parent it cannot see -- so a
    // hand-written file with the order reversed is refused rather than silently
    // losing the hierarchy.
    // WHO OWNS THE ORIGIN ON LOAD (v10). The constructor always makes one, and
    // a v10 file carries its own -- so restoring naively gave the loaded
    // document TWO frames named "Origin", one of them with an id no reference
    // in the file points at. Caught by the byte-identical round-trip tests,
    // which is what they are for.
    //
    // The file wins when it has frames: those are the ones every
    // `supportFrameId` refers to. A pre-v10 file has no frames array at all,
    // and keeps the constructor's Origin exactly as it always did -- so old
    // documents load byte-for-byte the way they did before M10.
    if (!frameData.empty())
        for (const ReferenceFrame* existing : document->frames())
            document->restoreRemoveObject(existing->id());
    // TWO PASSES, because file order does not guarantee parents first. The
    // first version of this comment claimed it did -- "guaranteed by writing
    // them in document order" -- and gate H refuted it immediately: frames are
    // written in CREATION order, and re-parenting a frame under one created
    // later puts the child first. Creating them all parentless and then wiring
    // the hierarchy is order-independent, and the cycle check still runs on the
    // second pass, so a cyclic file is still refused.
    docjson::restoreFramesAndConnectors(*document, frameData, connectorData);

    // Sketches first: restorePadFeature wires an edge to its Sketch node, so
    // the sketch must already exist in the registry and graph.
    for (auto& sketch : sketchData) {
        Sketch& restoredSketch = document->restoreSketch(sketch.id, std::move(sketch.name),
                                                         SketchFrame{sketch.transform});
        for (auto& entity : sketch.entities) {
            restoredSketch.restoreEntity(entity.id, std::move(entity.geometry));
            if (entity.construction) restoredSketch.setEntityConstruction(entity.id, true);
        }
        for (auto& constraint : sketch.constraints) {
            const SketchConstraintId restoredId = constraint.id;
            const bool restoredDriven = constraint.driven;
            restoredSketch.restoreConstraint(restoredId, std::move(constraint.data));
            // v20. Applied through the SKETCH rather than the document facade:
            // this is a restore, not an edit, and the facade's version records
            // an undo step for something the user did not just do.
            if (restoredDriven) restoredSketch.setConstraintDriven(restoredId, true);
        }
        // The underlay (v17). Restored like entities -- id kept, generator
        // advanced -- and deliberately NOT validated against the entities: a
        // reference is a frozen picture of a face, and it is allowed to have
        // nothing left in common with what the user has since drawn.
        for (auto& reference : sketch.references)
            restoredSketch.restoreReference(reference.id, std::move(reference.geometry));
        // AFTER the constraints, then swept. A placement naming a constraint
        // the file does not contain describes nothing, and keeping it would
        // let it re-attach itself to whatever later reused that id.
        for (const auto& placement : sketch.placements)
            restoredSketch.restoreDimensionPlacement(placement.constraintId, placement.labelMm);
        for (const auto& format : sketch.formats) {
            Sketch::DimensionFormat restored;
            restored.constraintId = format.constraintId;
            restored.prefix = format.prefix;
            restored.suffix = format.suffix;
            restored.plusTolerance = format.plusTolerance;
            restored.minusTolerance = format.minusTolerance;
            restoredSketch.restoreDimensionFormat(restored);
        }
        restoredSketch.dropPlacementsWithoutConstraints();
        // v10: back onto its support frame, recording no undo step.
        if (sketch.supportFrameId != kInvalidObjectId)
            document->restoreSketchSupportFrame(restoredSketch.id(), sketch.supportFrameId);
        // v19's tracked face is applied LATER -- see after the feature loop.
        // The facade refuses a query naming a feature that does not exist yet,
        // and features are restored after sketches, so applying it here
        // dropped it SILENTLY on every load. The file said the sketch followed
        // a face; the loaded document said it did not.

        // Parameter -> Sketch edges are OPTION B: re-derived from the
        // constraints, never written to the file (ADR-012's split, ADR-M5-008's
        // edge). Deriving them is what makes a hand-written or older file get
        // the same graph as a saved one, and it removes the possibility of a
        // file whose edge list disagrees with its own constraints.
        for (const SketchConstraint& constraint : restoredSketch.constraints()) {
            const ObjectId parameterId = BoundParameterId(constraint.data);
            if (parameterId == kInvalidObjectId) continue;
            document->addDependency(restoredSketch.id(), parameterId);
        }
    }
    for (auto& body : bodyData) {
        Body& restored = document->restoreBody(body.id, std::move(body.name));
        for (auto& feature : body.features) {
            // ONE dispatch, shared with M9's undo history (FeatureSnapshot.h).
            // This block used to be the loader's own per-type `if` chain, a
            // second copy of knowledge the save side above already holds; undo
            // needed the same knowledge a third time, and a third copy is
            // exactly this project's second recurring defect class. The chain
            // moved into `RestoreFeatureFromSnapshot` and both callers use it.
            RestoreFeatureFromSnapshot(*document, restored, feature);
        }
        // v19: the tracked faces, now that the features they name exist
        // (M17.14). Through the FACADE, so the graph edge comes with them: a
        // restored sketch holding the query with no dependency on the feature
        // would be clean when that feature moved, and would report the plane
        // from before -- the defect tracking exists to remove, reintroduced by
        // the loader.
        //
        // A REFUSAL FAILS THE LOAD. A file whose sketch says it follows a face
        // that cannot be followed is a file that would behave differently from
        // what it says, and dropping the tracking quietly is how it would.
        for (const auto& sketch : sketchData) {
            if (!sketch.trackedFace.has_value()) continue;
            if (!document->setSketchTrackedFace(sketch.id, *sketch.trackedFace))
                return loadFailure(SerializationError::InvalidFieldType,
                                   "sketch " + idToString(sketch.id) +
                                       ": its trackedFace names a feature that is not a solid, "
                                       "or one that would depend on this sketch");
        }

        // AFTER the features exist, so the position can be clamped against a
        // real count rather than a promised one, and so a file claiming a
        // rollback past the end of its own feature list simply evaluates
        // everything instead of being refused -- the position is a view, and a
        // view that has drifted is not a corrupt document.
        document->restoreRollbackPosition(restored.id(), body.rollback);
    }
    for (const EdgeData& edge : edgeData) {
        const GraphResult applied = document->addDependency(edge.dependent, edge.prerequisite);
        if (!applied) // defensive: pre-validated on the scratch graph
            return loadFailure(SerializationError::InvalidDependency,
                               "failed to re-apply dependency " + idToString(edge.prerequisite) +
                                   " -> " + idToString(edge.dependent));
    }
    return LoadResult{std::move(document), SerializationError::None, {}};
    } catch (const std::exception& error) {
        return loadFailure(SerializationError::DuplicateId,
                           std::string("document could not be restored: ") + error.what());
    }
}

SaveResult savePartDocumentToFile(const PartDocument& document, const std::string& path) {
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open())
        return SaveResult{SerializationError::IoError, "cannot open file for writing: " + path};
    return savePartDocument(document, file);
}

LoadResult loadPartDocumentFromFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return loadFailure(SerializationError::FileNotFound, "cannot open file: " + path);
    return loadPartDocument(file);
}

} // namespace paramcad
