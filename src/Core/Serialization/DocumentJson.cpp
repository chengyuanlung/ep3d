#include "Core/Serialization/DocumentJson.h"

#include "Core/Document/DocumentBase.h"
#include "Core/Reference/ReferenceFrame.h"

#include <charconv>
#include <fstream>
#include <sstream>
#include <utility>

namespace paramcad {
namespace docjson {

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


std::optional<DocumentType> documentTypeOfFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream buffer;
    buffer << in.rdbuf();

    JsonParseError parseError;
    const JsonValue root = parseJson(buffer.str(), parseError);
    if (parseError.ok == false || root.type() != JsonType::Object) return std::nullopt;

    // FORMAT FIRST. A JSON file that is not one of ours has no documentType to
    // read, and answering "Part" for it would send a caller down the part
    // loader to get a message about a missing field rather than about a file
    // that is not an EP3D document at all.
    const JsonValue* format = root.find("format");
    if (format == nullptr || format->type() != JsonType::String ||
        format->asString() != kFormatName)
        return std::nullopt;

    const JsonValue* type = root.find("documentType");
    if (type == nullptr || type->type() != JsonType::String) return std::nullopt;
    if (type->asString() == "Assembly") return DocumentType::Assembly;
    if (type->asString() == "Part") return DocumentType::Part;
    if (type->asString() == "Drawing") return DocumentType::Drawing;
    return std::nullopt;
}

JsonValue transformToJson(const Transform3D& transform) {
    JsonValue out = JsonValue::makeObject();
    out.set("tx", JsonValue::makeNumber(transform.translation.x));
    out.set("ty", JsonValue::makeNumber(transform.translation.y));
    out.set("tz", JsonValue::makeNumber(transform.translation.z));
    out.set("qw", JsonValue::makeNumber(transform.rotation.w));
    out.set("qx", JsonValue::makeNumber(transform.rotation.x));
    out.set("qy", JsonValue::makeNumber(transform.rotation.y));
    out.set("qz", JsonValue::makeNumber(transform.rotation.z));
    return out;
}

bool transformFromJson(const JsonValue& object, const std::string& context, FieldError& err,
                       Transform3D& out) {
    const auto number = [&](const char* key, double& into) {
        const JsonValue* field = object.find(key);
        if (field == nullptr || field->type() != JsonType::Number) return false;
        into = field->asNumber();
        return true;
    };
    Transform3D t;
    // ALL SEVEN OR NONE. A transform missing its qw reads as a rotation of
    // zero, which is not "no rotation" -- it is a degenerate quaternion, and a
    // part placed by it would arrive somewhere no one chose.
    if (!number("tx", t.translation.x) || !number("ty", t.translation.y) ||
        !number("tz", t.translation.z) || !number("qw", t.rotation.w) ||
        !number("qx", t.rotation.x) || !number("qy", t.rotation.y) ||
        !number("qz", t.rotation.z)) {
        err = fieldError(SerializationError::InvalidFieldType,
                         context + ": 'transform' is missing a numeric component");
        return false;
    }
    out = t;
    return true;
}

std::string_view toString(ConnectorRole role) {
    switch (role) {
        case ConnectorRole::Generic: return "Generic";
        case ConnectorRole::Mount: return "Mount";
        case ConnectorRole::Shaft: return "Shaft";
        case ConnectorRole::LinearGuide: return "LinearGuide";
        case ConnectorRole::ToolFlange: return "ToolFlange";
        case ConnectorRole::Electrical: return "Electrical";
        case ConnectorRole::Pneumatic: return "Pneumatic";
    }
    return "Generic";
}

std::optional<ConnectorRole> connectorRoleFromString(std::string_view text) {
    if (text == "Generic") return ConnectorRole::Generic;
    if (text == "Mount") return ConnectorRole::Mount;
    if (text == "Shaft") return ConnectorRole::Shaft;
    if (text == "LinearGuide") return ConnectorRole::LinearGuide;
    if (text == "ToolFlange") return ConnectorRole::ToolFlange;
    if (text == "Electrical") return ConnectorRole::Electrical;
    if (text == "Pneumatic") return ConnectorRole::Pneumatic;
    return std::nullopt;
}

std::string_view toString(ConnectorOwner owner) {
    return owner == ConnectorOwner::Assembly ? "Assembly" : "PartDefinition";
}

std::optional<ConnectorOwner> connectorOwnerFromString(std::string_view text) {
    if (text == "PartDefinition") return ConnectorOwner::PartDefinition;
    if (text == "Assembly") return ConnectorOwner::Assembly;
    return std::nullopt;
}

void writeHeader(JsonValue& root, std::string_view documentType, ObjectId id,
                 const std::string& name) {
    root.set("format", JsonValue::makeString(std::string(kFormatName)));
    root.set("schemaVersion", JsonValue::makeNumber(kSchemaVersion));
    root.set("documentType", JsonValue::makeString(std::string(documentType)));
    root.set("id", JsonValue::makeString(idToString(id)));
    root.set("name", JsonValue::makeString(name));
}

bool readHeader(const JsonValue& root, std::string_view expectedType, FieldError& err,
                ObjectId& id, std::string& name) {
    const std::string context = "document";
    const JsonValue* format = requireField(root, "format", JsonType::String, context, err);
    if (format == nullptr) return false;
    if (format->asString() != kFormatName) {
        err = fieldError(SerializationError::WrongFormat,
                         "unrecognized format '" + format->asString() + "'");
        return false;
    }

    // The version is a CEILING, not a content gate: a file stamped v6 that
    // carries a v8-only record type still loads, because every record is
    // validated by CONTENT. Refusing "newer records than the stamp admits"
    // would add a second source of truth about what the file contains, and the
    // two could disagree; the stamp's one job is to refuse files newer than
    // this LOADER.
    const JsonValue* schemaVersion =
        requireField(root, "schemaVersion", JsonType::Number, context, err);
    if (schemaVersion == nullptr) return false;
    const double value = schemaVersion->asNumber();
    if (value < static_cast<double>(kMinSupportedSchemaVersion) ||
        value > static_cast<double>(kSchemaVersion) ||
        value != static_cast<double>(static_cast<int>(value))) {
        err = fieldError(SerializationError::UnsupportedSchemaVersion,
                         "unsupported schema version " + std::to_string(value));
        return false;
    }

    const JsonValue* documentType =
        requireField(root, "documentType", JsonType::String, context, err);
    if (documentType == nullptr) return false;
    if (documentType->asString() != expectedType) {
        err = fieldError(SerializationError::WrongDocumentType,
                         "unsupported document type '" + documentType->asString() + "'");
        return false;
    }

    const JsonValue* idField = requireField(root, "id", JsonType::String, context, err);
    if (idField == nullptr) return false;
    const auto parsed = idFromString(idField->asString());
    if (!parsed.has_value() || *parsed == kInvalidObjectId || *parsed > kMaxObjectId) {
        err = fieldError(SerializationError::InvalidFieldType,
                         context + ": field 'id' is not a valid decimal ObjectId string");
        return false;
    }
    id = *parsed;

    const JsonValue* nameField = requireField(root, "name", JsonType::String, context, err);
    if (nameField == nullptr) return false;
    name = nameField->asString();
    return true;
}

JsonValue framesToJson(const DocumentBase& document) {
    // The Origin frame is written like any other. It is a real object with a
    // real id that other things reference, so leaving it out would mean
    // re-deriving it on load and hoping the id matched.
    JsonValue frames = JsonValue::makeArray();
    for (const ReferenceFrame* frame : document.frames()) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("id", JsonValue::makeString(idToString(frame->id())));
        entry.set("name", JsonValue::makeString(frame->name()));
        entry.set("parentFrameId", JsonValue::makeString(idToString(frame->parentFrameId())));
        entry.set("transform", transformToJson(frame->localTransform()));
        frames.add(std::move(entry));
    }
    return frames;
}

JsonValue connectorsToJson(const DocumentBase& document) {
    JsonValue connectors = JsonValue::makeArray();
    for (const Connector* connector : document.connectors()) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("id", JsonValue::makeString(idToString(connector->id())));
        entry.set("name", JsonValue::makeString(connector->name()));
        entry.set("role", JsonValue::makeString(std::string(toString(connector->role()))));
        entry.set("frameId", JsonValue::makeString(idToString(connector->frameId())));
        entry.set("owner", JsonValue::makeString(std::string(toString(connector->owner()))));
        connectors.add(std::move(entry));
    }
    return connectors;
}

namespace {

// The id of one array entry, claimed against the caller's uniqueness rule.
bool entryId(const JsonValue& entry, const std::string& context, const IdClaim& claimId,
             FieldError& err, ObjectId& out) {
    const JsonValue* field = requireField(entry, "id", JsonType::String, context, err);
    if (field == nullptr) return false;
    // The messages are the Part loader's, word for word: several accepted
    // tests pin them, and two spellings of one refusal is the drift this whole
    // header exists to prevent.
    const auto parsed = idFromString(field->asString());
    if (!parsed.has_value() || *parsed == kInvalidObjectId) {
        err = fieldError(SerializationError::InvalidFieldType,
                         context + ": field 'id' is not a valid decimal ObjectId string");
        return false;
    }
    if (*parsed > kMaxObjectId) {
        err = fieldError(SerializationError::InvalidFieldType,
                         context + ": field 'id' value " + field->asString() +
                             " exceeds the maximum ObjectId (2^63 - 1)");
        return false;
    }
    if (!claimId(*parsed, context, err)) return false;
    out = *parsed;
    return true;
}

} // namespace

bool readFrames(const JsonValue& root, const IdClaim& claimId, FieldError& err,
                std::vector<FrameData>& out) {
    const JsonValue* field = root.find("frames");
    if (field == nullptr) return true;
    if (field->type() != JsonType::Array) {
        err = fieldError(SerializationError::InvalidFieldType,
                         "document: field 'frames' is not an array");
        return false;
    }
    for (std::size_t i = 0; i < field->items().size(); ++i) {
        const JsonValue& entry = field->items()[i];
        const std::string context = "frames[" + std::to_string(i) + "]";
        if (entry.type() != JsonType::Object) {
            err = fieldError(SerializationError::InvalidFieldType,
                             context + ": entry is not an object");
            return false;
        }
        FrameData frame;
        if (!entryId(entry, context, claimId, err, frame.id)) return false;
        const JsonValue* name = requireField(entry, "name", JsonType::String, context, err);
        if (name == nullptr) return false;
        frame.name = name->asString();
        if (const JsonValue* parent = entry.find("parentFrameId")) {
            if (parent->type() != JsonType::String) {
                err = fieldError(SerializationError::InvalidFieldType,
                                 context + ": field 'parentFrameId' is not a string");
                return false;
            }
            const auto parsed = idFromString(parent->asString());
            if (parsed.has_value()) frame.parentFrameId = *parsed;
        }
        if (const JsonValue* transform = entry.find("transform")) {
            if (transform->type() != JsonType::Object) {
                err = fieldError(SerializationError::InvalidFieldType,
                                 context + ": field 'transform' is not an object");
                return false;
            }
            if (!transformFromJson(*transform, context, err, frame.transform)) return false;
        }
        out.push_back(std::move(frame));
    }
    return true;
}

bool readConnectors(const JsonValue& root, const IdClaim& claimId, FieldError& err,
                    std::vector<ConnectorData>& out) {
    const JsonValue* field = root.find("connectors");
    if (field == nullptr) return true;
    if (field->type() != JsonType::Array) {
        err = fieldError(SerializationError::InvalidFieldType,
                         "document: field 'connectors' is not an array");
        return false;
    }
    for (std::size_t i = 0; i < field->items().size(); ++i) {
        const JsonValue& entry = field->items()[i];
        const std::string context = "connectors[" + std::to_string(i) + "]";
        if (entry.type() != JsonType::Object) {
            err = fieldError(SerializationError::InvalidFieldType,
                             context + ": entry is not an object");
            return false;
        }
        ConnectorData connector;
        if (!entryId(entry, context, claimId, err, connector.id)) return false;
        const JsonValue* name = requireField(entry, "name", JsonType::String, context, err);
        if (name == nullptr) return false;
        connector.name = name->asString();
        const JsonValue* role = requireField(entry, "role", JsonType::String, context, err);
        if (role == nullptr) return false;
        const auto roleValue = connectorRoleFromString(role->asString());
        if (!roleValue.has_value()) {
            err = fieldError(SerializationError::InvalidEnumValue,
                             context + ": unknown connector role '" + role->asString() + "'");
            return false;
        }
        connector.role = *roleValue;
        const JsonValue* frameId = requireField(entry, "frameId", JsonType::String, context, err);
        if (frameId == nullptr) return false;
        const auto frameValue = idFromString(frameId->asString());
        if (!frameValue.has_value()) {
            err = fieldError(SerializationError::InvalidFieldType,
                             context + ": field 'frameId' is not a valid ObjectId");
            return false;
        }
        connector.frameId = *frameValue;
        if (const JsonValue* ownerField = entry.find("owner")) {
            if (ownerField->type() != JsonType::String) {
                err = fieldError(SerializationError::InvalidFieldType,
                                 context + ": field 'owner' is not a string");
                return false;
            }
            const auto parsed = connectorOwnerFromString(ownerField->asString());
            if (!parsed.has_value()) {
                err = fieldError(SerializationError::InvalidEnumValue,
                                 context + ": unknown connector owner '" +
                                     ownerField->asString() + "'");
                return false;
            }
            connector.owner = *parsed;
        }
        out.push_back(std::move(connector));
    }
    return true;
}

void restoreFramesAndConnectors(DocumentBase& document, std::vector<FrameData>& frames,
                                std::vector<ConnectorData>& connectors) {
    for (auto& frame : frames)
        document.restoreFrame(frame.id, std::move(frame.name), kInvalidObjectId, frame.transform);
    for (const auto& frame : frames)
        if (frame.parentFrameId != kInvalidObjectId)
            document.restoreFrameParent(frame.id, frame.parentFrameId);
    for (auto& connector : connectors)
        document.restoreConnector(connector.id, std::move(connector.name), connector.role,
                                  connector.frameId, connector.owner);
}

} // namespace docjson
} // namespace paramcad
