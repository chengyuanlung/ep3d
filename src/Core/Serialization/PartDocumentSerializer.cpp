#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Dependency/DependencyGraph.h"
#include "Core/Feature/BoxFeature.h"
#include "Core/Feature/IMaterialReferencing.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/PocketFeature.h"
#include "Core/Feature/RevolveFeature.h"
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

constexpr int kSchemaVersion = 8;           // v8 adds Fillet and Chamfer (M8.3)
constexpr int kMinSupportedSchemaVersion = 1; // v1 (no edges) and v2 files still load
constexpr std::string_view kFormatName = "ParametricCAD";

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
    }
    return "Whole";
}

std::optional<SketchSubElement> subElementFromString(const std::string& name) {
    if (name == "Whole") return SketchSubElement::Whole;
    if (name == "StartPoint") return SketchSubElement::StartPoint;
    if (name == "EndPoint") return SketchSubElement::EndPoint;
    if (name == "CenterPoint") return SketchSubElement::CenterPoint;
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

std::string idToString(ObjectId id) {
    char buffer[24];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), id);
    return std::string(buffer, result.ptr);
}

std::optional<ObjectId> idFromString(std::string_view text) {
    if (text.empty()) return std::nullopt;
    ObjectId id = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, id);
    if (result.ec != std::errc{} || result.ptr != last) return std::nullopt;
    return id;
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
                featureEntry.set("materialId",
                                 JsonValue::makeString(idToString(dress->materialId())));
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

        JsonValue entities = JsonValue::makeArray();
        for (const SketchEntity& entity : sketch->entities()) {
            JsonValue entry = JsonValue::makeObject();
            entry.set("id", JsonValue::makeString(idToString(ToObjectId(entity.id))));
            std::visit(
                [&entry](const auto& geometry) {
                    using T = std::decay_t<decltype(geometry)>;
                    if constexpr (std::is_same_v<T, SketchPoint>) {
                        entry.set("type", JsonValue::makeString("Point"));
                        entry.set("u", JsonValue::makeNumber(geometry.position.x));
                        entry.set("v", JsonValue::makeNumber(geometry.position.y));
                    } else if constexpr (std::is_same_v<T, SketchLine>) {
                        entry.set("type", JsonValue::makeString("Line"));
                        entry.set("u1", JsonValue::makeNumber(geometry.start.x));
                        entry.set("v1", JsonValue::makeNumber(geometry.start.y));
                        entry.set("u2", JsonValue::makeNumber(geometry.end.x));
                        entry.set("v2", JsonValue::makeNumber(geometry.end.y));
                    } else if constexpr (std::is_same_v<T, SketchCircle>) {
                        entry.set("type", JsonValue::makeString("Circle"));
                        entry.set("u", JsonValue::makeNumber(geometry.center.x));
                        entry.set("v", JsonValue::makeNumber(geometry.center.y));
                        entry.set("radiusMm", JsonValue::makeNumber(geometry.radiusMm));
                    } else {
                        static_assert(std::is_same_v<T, SketchArc>);
                        entry.set("type", JsonValue::makeString("Arc"));
                        entry.set("u", JsonValue::makeNumber(geometry.center.x));
                        entry.set("v", JsonValue::makeNumber(geometry.center.y));
                        entry.set("radiusMm", JsonValue::makeNumber(geometry.radiusMm));
                        entry.set("startAngleRad", JsonValue::makeNumber(geometry.startAngleRad));
                        entry.set("endAngleRad", JsonValue::makeNumber(geometry.endAngleRad));
                        entry.set("counterClockwise",
                                  JsonValue::makeBool(geometry.counterClockwise));
                    }
                },
                entity.geometry);
            entities.add(std::move(entry));
        }
        sketchEntry.set("entities", std::move(entities));

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
                    } else if constexpr (std::is_same_v<T, FixConstraint>) {
                        writeRef("target", c.target);
                    } else if constexpr (std::is_same_v<T, DistanceConstraint>) {
                        writeRef("a", c.a);
                        writeRef("b", c.b);
                    } else if constexpr (std::is_same_v<T, LengthConstraint>) {
                        writeEntity("line", c.line);
                    } else if constexpr (std::is_same_v<T, RadiusConstraint> ||
                                         std::is_same_v<T, DiameterConstraint>) {
                        writeEntity("curve", c.curve);
                    } else {
                        static_assert(std::is_same_v<T, AngleConstraint>);
                        writeEntity("lineA", c.lineA);
                        writeEntity("lineB", c.lineB);
                    }
                },
                constraint.data);

            const ObjectId parameterId = BoundParameterId(constraint.data);
            if (parameterId != kInvalidObjectId)
                entry.set("parameterId", JsonValue::makeString(idToString(parameterId)));
            constraints.add(std::move(entry));
        }
        sketchEntry.set("constraints", std::move(constraints));
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
    root.set("dependencies", std::move(dependencies));
    return root;
}

// --- load helpers -----------------------------------------------------------

struct FieldError {
    SerializationError error = SerializationError::None;
    std::string message;
    bool ok() const noexcept { return error == SerializationError::None; }
};

FieldError fieldError(SerializationError error, std::string message) {
    return FieldError{error, std::move(message)};
}

const JsonValue* requireField(const JsonValue& object, std::string_view key,
                              JsonType expectedType, const std::string& context,
                              FieldError& err) {
    const JsonValue* field = object.find(key);
    if (field == nullptr) {
        err = fieldError(SerializationError::MissingField,
                         context + ": missing required field '" + std::string(key) + "'");
        return nullptr;
    }
    if (field->type() != expectedType) {
        err = fieldError(SerializationError::InvalidFieldType,
                         context + ": field '" + std::string(key) + "' has the wrong JSON type");
        return nullptr;
    }
    return field;
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
    if (const SaveResult bad = capCheck(document.id(), "the document"); !bad) return bad;
    for (const auto& parameter : document.parameters().items())
        if (const SaveResult bad = capCheck(parameter->id(), "a parameter"); !bad) return bad;
    for (const auto& body : document.bodies()) {
        if (const SaveResult bad = capCheck(body->id(), "a body"); !bad) return bad;
        for (const auto& feature : body->features())
            if (const SaveResult bad = capCheck(feature->id(), "a feature"); !bad) return bad;
    }
    for (const Sketch* sketch : document.sketches()) {
        if (const SaveResult bad = capCheck(sketch->id(), "a sketch"); !bad) return bad;
        for (const SketchEntity& entity : sketch->entities())
            if (const SaveResult bad = capCheck(ToObjectId(entity.id), "a sketch entity"); !bad)
                return bad;
        for (const SketchConstraint& constraint : sketch->constraints())
            if (const SaveResult bad = capCheck(ToObjectId(constraint.id), "a sketch constraint");
                !bad)
                return bad;
    }
    if (document.material() != nullptr)
        if (const SaveResult bad = capCheck(document.material()->id(), "the material"); !bad)
            return bad;

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
            if (dynamic_cast<const BoxFeature*>(feature.get()) != nullptr) continue;
            if (dynamic_cast<const PadFeature*>(feature.get()) != nullptr) continue;
            if (dynamic_cast<const PocketFeature*>(feature.get()) != nullptr) continue;
            if (dynamic_cast<const RevolveFeature*>(feature.get()) != nullptr) continue;
            if (dynamic_cast<const EdgeDressFeature*>(feature.get()) != nullptr) continue;
            const std::string_view typeName = feature->typeName();
            if (typeName != "Box" && typeName != "Pad" && typeName != "Pocket" &&
                typeName != "Revolve" && typeName != "Fillet" && typeName != "Chamfer")
                continue;
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
            const ObjectId consumedBase =
                solid != nullptr ? solid->consumedSolidId() : kInvalidObjectId;
            if (consumedBase != kInvalidObjectId) {
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
                                          "may be consumed once (ADR-M8-001)"};
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
    return SaveResult{};
}

} // namespace

// --- public API -------------------------------------------------------------

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
    struct FeatureData {
        ObjectId id;
        std::string name;
        std::string type;
        ComputeState state;
        // Box-specific (ADR-M3-005; only meaningful when type == "Box").
        ObjectId widthParameterId = kInvalidObjectId;
        ObjectId heightParameterId = kInvalidObjectId;
        ObjectId depthParameterId = kInvalidObjectId;
        ObjectId materialId = kInvalidObjectId; // kInvalidObjectId == "no material"
        // Pad-specific (v4, ADR-M4-004; only meaningful when type == "Pad").
        ObjectId sketchId = kInvalidObjectId;
        ObjectId lengthParameterId = kInvalidObjectId;
        // Pocket-specific (v6, ADR-M8-001; only meaningful when type == "Pocket").
        // depthParameterId is SHARED with the Box block above -- one slot, only
        // ever meaningful for the type the record declares, like every other
        // per-type field here.
        ObjectId baseFeatureId = kInvalidObjectId;
        // Revolve-specific (v7, ADR-M8-005; only meaningful when type == "Revolve").
        ObjectId axisEntityId = kInvalidObjectId;
        ObjectId angleParameterId = kInvalidObjectId;
        // Fillet/Chamfer-specific (v8, ADR-M8-006).
        ObjectId sizeParameterId = kInvalidObjectId;
    };
    struct BodyData {
        ObjectId id;
        std::string name;
        std::vector<FeatureData> features;
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

        BodyData body{*id, bodyName->asString(), {}};
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
            if (featureData.type == "Box") {
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
            } else if (featureData.type == "Pad") {
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
            } else if (featureData.type == "Pocket") {
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
            } else if (featureData.type == "Revolve") {
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
            } else if (featureData.type == "Fillet" || featureData.type == "Chamfer") {
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
    };
    struct SketchConstraintData_ {
        SketchConstraintId id{kInvalidSketchConstraintId};
        SketchConstraintData data{};
    };
    struct SketchData {
        ObjectId id;
        std::string name;
        Transform3D transform;
        std::vector<SketchEntityData> entities;
        std::vector<SketchConstraintData_> constraints;
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

                const JsonValue* typeField =
                    requireField(entityEntry, "type", JsonType::String, entityContext, err);
                if (typeField == nullptr) return loadFailure(err.error, err.message);
                const std::string entityType = typeField->asString();

                double u = 0.0, v = 0.0, u2 = 0.0, v2 = 0.0, radius = 0.0;
                double startAngle = 0.0, endAngle = 0.0;
                const auto num = [&](const char* key, double& out) -> bool {
                    const JsonValue* field =
                        requireField(entityEntry, key, JsonType::Number, entityContext, err);
                    if (field == nullptr) return false;
                    out = field->asNumber();
                    return true;
                };

                SketchEntityData entity;
                entity.id = static_cast<SketchEntityId>(*entityId);
                if (entityType == "Point") {
                    if (!num("u", u) || !num("v", v)) return loadFailure(err.error, err.message);
                    entity.geometry = SketchPoint{Vec2{u, v}};
                } else if (entityType == "Line") {
                    if (!num("u1", u) || !num("v1", v) || !num("u2", u2) || !num("v2", v2))
                        return loadFailure(err.error, err.message);
                    entity.geometry = SketchLine{Vec2{u, v}, Vec2{u2, v2}};
                } else if (entityType == "Circle") {
                    if (!num("u", u) || !num("v", v) || !num("radiusMm", radius))
                        return loadFailure(err.error, err.message);
                    entity.geometry = SketchCircle{Vec2{u, v}, radius};
                } else if (entityType == "Arc") {
                    if (!num("u", u) || !num("v", v) || !num("radiusMm", radius) ||
                        !num("startAngleRad", startAngle) || !num("endAngleRad", endAngle))
                        return loadFailure(err.error, err.message);
                    const JsonValue* ccwField = requireField(entityEntry, "counterClockwise",
                                                             JsonType::Bool, entityContext, err);
                    if (ccwField == nullptr) return loadFailure(err.error, err.message);
                    entity.geometry =
                        SketchArc{Vec2{u, v}, radius, startAngle, endAngle, ccwField->asBool()};
                } else {
                    return loadFailure(SerializationError::InvalidEnumValue,
                                       entityContext + ": unknown sketch entity type '" +
                                           entityType + "'");
                }
                data.entities.push_back(std::move(entity));
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
                        return SketchElementRef{static_cast<SketchEntityId>(*id), *sub};
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
                    } else if (kind == "Fix") {
                        parsed.data = FixConstraint{elementRef("target")};
                    } else if (kind == "Distance") {
                        DistanceConstraint c;
                        c.a = elementRef("a");
                        c.b = elementRef("b");
                        c.parameterId = parameterRef();
                        parsed.data = c;
                    } else if (kind == "Length") {
                        LengthConstraint c;
                        c.line = entityRef("line");
                        c.parameterId = parameterRef();
                        parsed.data = c;
                    } else if (kind == "Radius") {
                        RadiusConstraint c;
                        c.curve = entityRef("curve");
                        c.parameterId = parameterRef();
                        parsed.data = c;
                    } else if (kind == "Diameter") {
                        DiameterConstraint c;
                        c.curve = entityRef("curve");
                        c.parameterId = parameterRef();
                        parsed.data = c;
                    } else if (kind == "Angle") {
                        AngleConstraint c;
                        c.lineA = entityRef("lineA");
                        c.lineB = entityRef("lineB");
                        c.parameterId = parameterRef();
                        parsed.data = c;
                    } else {
                        return loadFailure(SerializationError::InvalidEnumValue,
                                           cc + ": unknown sketch constraint type '" + kind + "'");
                    }
                    if (refError) return loadFailure(refCode, refMessage);
                    data.constraints.push_back(std::move(parsed));
                }
            }

            sketchData.push_back(std::move(data));
        }
    }

    // Every Pad must reference a Sketch present in this file. Same rule as the
    // Box parameter check above: a reference the loader would reject must never
    // have been savable either (ADR-M3-008).
    for (const auto& body : bodyData) {
        for (const auto& feature : body.features) {
            if (feature.type != "Pad") continue;
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
    // validateSaveable enforces the same rules at save time; both halves exist
    // so neither can drift alone.
    static const std::unordered_set<std::string> kSolidFeatureTypes{
        "Box", "Pad", "Pocket", "Revolve", "Fillet", "Chamfer"};
    for (const auto& body : bodyData) {
        std::unordered_set<ObjectId> earlierSolids;
        std::unordered_set<ObjectId> consumedBases;
        for (const auto& feature : body.features) {
            const bool consumes = feature.type == "Pocket" || feature.type == "Fillet" ||
                                  feature.type == "Chamfer";
            if (feature.type == "Pocket" && sketchIds.count(feature.sketchId) == 0)
                return loadFailure(SerializationError::UnknownDependencyId,
                                   "feature " + idToString(feature.id) +
                                       ": pocket sketch id " + idToString(feature.sketchId) +
                                       " is not a sketch in this document");
            if (consumes) {
                const std::string noun =
                    feature.type == "Pocket" ? "pocket" : "fillet/chamfer";
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
                                           "solid may be consumed once (ADR-M8-001)");
            }
            if (kSolidFeatureTypes.count(feature.type) != 0) earlierSolids.insert(feature.id);
        }
    }

    // Every Revolve must reference a Sketch in this file, and its axis must be
    // an entity OF that sketch -- an axis id that resolves to nothing, or to an
    // entity of a different sketch, is a file the saver could never have
    // written (ADR-M3-008).
    for (const auto& body : bodyData) {
        for (const auto& feature : body.features) {
            if (feature.type != "Revolve") continue;
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
    if (materialData.has_value()) {
        document->restoreMaterial(materialData->id, std::move(materialData->name),
                                  materialData->densityKgPerM3, materialData->elasticModulusPa,
                                  materialData->poissonRatio, materialData->yieldStrengthPa,
                                  materialData->contact);
    }
    // Sketches first: restorePadFeature wires an edge to its Sketch node, so
    // the sketch must already exist in the registry and graph.
    for (auto& sketch : sketchData) {
        Sketch& restoredSketch = document->restoreSketch(sketch.id, std::move(sketch.name),
                                                         SketchFrame{sketch.transform});
        for (auto& entity : sketch.entities)
            restoredSketch.restoreEntity(entity.id, std::move(entity.geometry));
        for (auto& constraint : sketch.constraints)
            restoredSketch.restoreConstraint(constraint.id, std::move(constraint.data));

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
            if (feature.type == "Box") {
                document->restoreBoxFeature(restored, feature.id, std::move(feature.name),
                                            feature.state, feature.widthParameterId,
                                            feature.heightParameterId, feature.depthParameterId,
                                            feature.materialId);
            } else if (feature.type == "Pad") {
                document->restorePadFeature(restored, feature.id, std::move(feature.name),
                                            feature.state, feature.sketchId,
                                            feature.lengthParameterId, feature.materialId);
            } else if (feature.type == "Pocket") {
                document->restorePocketFeature(restored, feature.id, std::move(feature.name),
                                               feature.state, feature.baseFeatureId,
                                               feature.sketchId, feature.depthParameterId,
                                               feature.materialId);
            } else if (feature.type == "Revolve") {
                document->restoreRevolveFeature(
                    restored, feature.id, std::move(feature.name), feature.state,
                    feature.sketchId, static_cast<SketchEntityId>(feature.axisEntityId),
                    feature.angleParameterId, feature.materialId);
            } else if (feature.type == "Fillet") {
                document->restoreFilletFeature(restored, feature.id, std::move(feature.name),
                                               feature.state, feature.baseFeatureId,
                                               feature.sizeParameterId, feature.materialId);
            } else if (feature.type == "Chamfer") {
                document->restoreChamferFeature(restored, feature.id, std::move(feature.name),
                                                feature.state, feature.baseFeatureId,
                                                feature.sizeParameterId, feature.materialId);
            } else {
                restored.addFeature<PlaceholderFeature>(feature.id, std::move(feature.name),
                                                        feature.state, std::move(feature.type));
            }
        }
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
