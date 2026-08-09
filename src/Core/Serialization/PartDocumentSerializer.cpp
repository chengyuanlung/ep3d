#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Dependency/DependencyGraph.h"
#include "Core/Feature/BoxFeature.h"
#include "Core/Feature/IMaterialReferencing.h"
#include "Core/Feature/PadFeature.h"
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
#include <string_view>
#include <unordered_set>
#include <vector>

namespace paramcad {

namespace {

constexpr int kSchemaVersion = 4;           // written on save (v4 adds Sketch/Pad)
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
            } else if (const auto* pad = dynamic_cast<const PadFeature*>(feature.get())) {
                // Semantic references only (ADR-M4-004): the Sketch, the Length
                // Parameter and the Material, each by ObjectId. No OCCT
                // topology, no index, no address is ever written.
                featureEntry.set("sketchId", JsonValue::makeString(idToString(pad->sketchId())));
                featureEntry.set("lengthParameterId",
                                 JsonValue::makeString(idToString(pad->lengthParameterId())));
                featureEntry.set("materialId", JsonValue::makeString(idToString(pad->materialId())));
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
            const std::string_view typeName = feature->typeName();
            if (typeName != "Box" && typeName != "Pad") continue;
            return SaveResult{SerializationError::InvalidFieldType,
                              "feature " + idToString(feature->id()) + " (" + feature->name() +
                                  ") is a placeholder carrying the reserved type name '" +
                                  std::string(typeName) +
                                  "'; the resulting file could never be loaded back"};
        }
    }

    std::unordered_set<ObjectId> sketchIds;
    for (const Sketch* sketch : document.sketches()) sketchIds.insert(sketch->id());

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
        // Pad-specific (v4, ADR-M4-004; only meaningful when type == "Pad").
        ObjectId sketchId = kInvalidObjectId;
        ObjectId lengthParameterId = kInvalidObjectId;
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
    struct SketchData {
        ObjectId id;
        std::string name;
        Transform3D transform;
        std::vector<SketchEntityData> entities;
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
    // Sketches first: restorePadFeature wires an edge to its Sketch node, so
    // the sketch must already exist in the registry and graph.
    for (auto& sketch : sketchData) {
        Sketch& restoredSketch = document->restoreSketch(sketch.id, std::move(sketch.name),
                                                         SketchFrame{sketch.transform});
        for (auto& entity : sketch.entities)
            restoredSketch.restoreEntity(entity.id, std::move(entity.geometry));
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
