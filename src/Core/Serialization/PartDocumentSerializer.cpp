#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Dependency/DependencyGraph.h"
#include "Core/Feature/BoxFeature.h"
#include "Core/Feature/PlaceholderFeature.h"
#include "Core/Material/Material.h"
#include "Core/Serialization/JsonValue.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <optional>
#include <ostream>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace paramcad {

namespace {

constexpr int kSchemaVersion = 3;           // written on save
constexpr int kMinSupportedSchemaVersion = 1; // v1 (no edges) and v2 files still load
constexpr std::string_view kFormatName = "ParametricCAD";

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
            }
            features.add(std::move(featureEntry));
        }
        bodyEntry.set("features", std::move(features));
        bodies.add(std::move(bodyEntry));
    }
    root.set("bodies", std::move(bodies));

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
    std::unordered_set<ObjectId> parameterIds;
    for (const auto& parameter : document.parameters().items())
        parameterIds.insert(parameter->id());

    for (const auto& body : document.bodies()) {
        for (const auto& feature : body->features()) {
            const auto* box = dynamic_cast<const BoxFeature*>(feature.get());
            if (box == nullptr) continue;
            for (ObjectId referenced :
                 {box->widthParameterId(), box->heightParameterId(), box->depthParameterId()}) {
                if (parameterIds.count(referenced) != 0) continue;
                return SaveResult{SerializationError::UnknownDependencyId,
                                  "feature " + idToString(box->id()) + " (" + box->name() +
                                      "): box parameter id " + idToString(referenced) +
                                      " is not a parameter in this document"};
            }
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
            }

            body.features.push_back(std::move(featureData));
        }
        bodyData.push_back(std::move(body));
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
    // first: restore allocates fresh ids along the way (the Origin frame in
    // the PartDocument restore ctor), and those must not collide with
    // persisted ids restored later. Done only after all validation succeeded,
    // preserving the never-advance-on-failure guarantee. The per-restore
    // RestoreObjectId calls remain as defense in depth.
    ObjectId maxPersistedId = *documentId;
    for (const auto& parameter : parameterData)
        maxPersistedId = std::max(maxPersistedId, parameter.id);
    if (materialData.has_value()) maxPersistedId = std::max(maxPersistedId, materialData->id);
    for (const auto& body : bodyData) {
        maxPersistedId = std::max(maxPersistedId, body.id);
        for (const auto& feature : body.features)
            maxPersistedId = std::max(maxPersistedId, feature.id);
    }
    ObjectIdGenerator::AdvancePast(maxPersistedId);

    // All restore goes through the document facade so ObjectRegistry and
    // DependencyGraph are rebuilt during load (M2-SER-003). Graph states are
    // not persisted: every restored node starts Dirty. Order matters: Material
    // before bodies/features, since a BoxFeature's Material->MassPropertiesNode
    // edge (Option B, ADR-M3-005) requires the Material to already be a graph
    // node when restoreBoxFeature wires it.
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
    for (auto& body : bodyData) {
        Body& restored = document->restoreBody(body.id, std::move(body.name));
        for (auto& feature : body.features) {
            if (feature.type == "Box") {
                document->restoreBoxFeature(restored, feature.id, std::move(feature.name),
                                            feature.state, feature.widthParameterId,
                                            feature.heightParameterId, feature.depthParameterId,
                                            feature.materialId);
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
