#include "Core/Serialization/AssemblyDocumentSerializer.h"

#include "Core/Serialization/DocumentJson.h"

#include <fstream>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace paramcad {

namespace {

using docjson::FieldError;
using docjson::fieldError;
using docjson::idFromString;
using docjson::idToString;
using docjson::requireField;

AssemblyLoadResult loadFailure(SerializationError error, std::string message) {
    return AssemblyLoadResult{nullptr, error, std::move(message)};
}

// EVERY REFERENCE THE LOADER CHECKS, CHECKED AT SAVE TIME (ADR-M3-008).
//
// The named worst case in this project is a document that saves cleanly and
// then refuses to load: the good file on disk is already gone by the time
// anybody finds out. So the rules below are exactly the loader's rules,
// applied before a byte is written.
SaveResult validateSaveable(const AssemblyDocument& document) {
    if (document.id() > kMaxObjectId)
        return SaveResult{SerializationError::InvalidFieldType,
                          "document id exceeds the maximum ObjectId this format can carry"};

    std::unordered_set<ObjectId> seen{document.id()};
    const auto claim = [&seen](ObjectId id, const char* what) -> SaveResult {
        if (id > kMaxObjectId)
            return SaveResult{SerializationError::InvalidFieldType,
                              std::string(what) + " id exceeds the maximum ObjectId this "
                                                  "format can carry"};
        if (!seen.insert(id).second)
            return SaveResult{SerializationError::DuplicateId,
                              std::string(what) + " id " + idToString(id) +
                                  " is used twice in this document"};
        return SaveResult{};
    };

    for (const ReferenceFrame* frame : document.frames())
        if (const SaveResult bad = claim(frame->id(), "frame"); !bad) return bad;
    for (const Connector* connector : document.connectors()) {
        if (const SaveResult bad = claim(connector->id(), "connector"); !bad) return bad;
        if (document.findFrame(connector->frameId()) == nullptr)
            return SaveResult{SerializationError::UnknownDependencyId,
                              "connector '" + connector->name() +
                                  "' names a frame that is not in this document"};
    }
    for (const PartInstance* instance : document.instances()) {
        if (const SaveResult bad = claim(instance->id(), "instance"); !bad) return bad;
        // An instance with no file can never build, so it can never be
        // anything but a Failed node in the tree. Refused where the reason is
        // near the cause.
        if (instance->sourcePath().empty())
            return SaveResult{SerializationError::MissingField,
                              "instance '" + instance->name() + "' names no part file"};
        if (document.findFrame(instance->frameId()) == nullptr)
            return SaveResult{SerializationError::UnknownDependencyId,
                              "instance '" + instance->name() +
                                  "' is placed by a frame that is not in this document"};
    }
    return SaveResult{};
}

JsonValue toJson(const AssemblyDocument& document) {
    JsonValue root = JsonValue::makeObject();
    docjson::writeHeader(root, "Assembly", document.id(), document.name());
    root.set("frames", docjson::framesToJson(document));
    root.set("connectors", docjson::connectorsToJson(document));

    // THE ONE ARRAY THAT IS NEW. A path, a body NAME and a frame id -- the
    // sentence "that body, in that file, over there". No geometry (there is
    // none in this format at all, ADR-M4-004) and no body index, because an
    // index would mean a different body the day the part gained one.
    //
    // Notice what is ALSO not here: the placement. It is the frame's
    // transform, in the frames array, once -- so a file cannot carry two
    // answers about where an instance is.
    JsonValue instances = JsonValue::makeArray();
    for (const PartInstance* instance : document.instances()) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("id", JsonValue::makeString(idToString(instance->id())));
        entry.set("name", JsonValue::makeString(instance->name()));
        entry.set("sourcePath", JsonValue::makeString(instance->sourcePath()));
        entry.set("bodyName", JsonValue::makeString(instance->bodyName()));
        entry.set("frameId", JsonValue::makeString(idToString(instance->frameId())));
        instances.add(std::move(entry));
    }
    root.set("instances", std::move(instances));
    return root;
}

struct InstanceData {
    ObjectId id = kInvalidObjectId;
    std::string name;
    std::string sourcePath;
    std::string bodyName;
    ObjectId frameId = kInvalidObjectId;
};

} // namespace

SaveResult saveAssemblyDocument(const AssemblyDocument& document, std::ostream& out) {
    if (const SaveResult invalid = validateSaveable(document); !invalid) return invalid;
    out << writeJson(toJson(document)) << '\n';
    if (!out.good())
        return SaveResult{SerializationError::IoError, "failed to write document to stream"};
    return SaveResult{};
}

AssemblyLoadResult loadAssemblyDocument(std::istream& in) {
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad())
        return loadFailure(SerializationError::IoError, "failed to read document from stream");

    JsonParseError parseError;
    const JsonValue root = parseJson(buffer.str(), parseError);
    if (!parseError.ok) {
        std::ostringstream message;
        message << "malformed JSON at line " << parseError.line << ", column "
                << parseError.column << ": " << parseError.message;
        return loadFailure(SerializationError::MalformedJson, message.str());
    }
    if (root.type() != JsonType::Object)
        return loadFailure(SerializationError::MalformedJson,
                           "top-level JSON value is not an object");

    FieldError err;
    ObjectId documentId = kInvalidObjectId;
    std::string documentName;
    if (!docjson::readHeader(root, "Assembly", err, documentId, documentName))
        return loadFailure(err.error, err.message);

    // Stable-identity rule: every persistent id in a document is unique across
    // all categories. The SET differs from a Part's -- instances rather than
    // features -- and the rule does not, which is why DocumentJson takes this
    // as a parameter instead of owning it.
    std::unordered_set<ObjectId> seenIds{documentId};
    const docjson::IdClaim registerId = [&seenIds](ObjectId id, const std::string& context,
                                                   FieldError& fieldErr) {
        if (!seenIds.insert(id).second) {
            fieldErr = fieldError(SerializationError::DuplicateId,
                                  context + ": duplicate ObjectId " + idToString(id) +
                                      " already used elsewhere in this document");
            return false;
        }
        return true;
    };

    std::vector<docjson::FrameData> frameData;
    std::vector<docjson::ConnectorData> connectorData;
    if (!docjson::readFrames(root, registerId, err, frameData))
        return loadFailure(err.error, err.message);
    if (!docjson::readConnectors(root, registerId, err, connectorData))
        return loadFailure(err.error, err.message);

    // EVERYTHING IS READ BEFORE ANYTHING IS BUILT (ADR-M3-002): a half
    // restored document is never handed back, so a file that fails validation
    // at the last instance leaves the caller with nothing rather than with
    // something that looks usable.
    std::vector<InstanceData> instanceData;
    const JsonValue* instances = requireField(root, "instances", JsonType::Array, "document", err);
    if (instances == nullptr) return loadFailure(err.error, err.message);
    for (std::size_t i = 0; i < instances->items().size(); ++i) {
        const JsonValue& entry = instances->items()[i];
        const std::string context = "instances[" + std::to_string(i) + "]";
        if (entry.type() != JsonType::Object)
            return loadFailure(SerializationError::InvalidFieldType,
                               context + ": entry is not an object");
        const JsonValue* idField = requireField(entry, "id", JsonType::String, context, err);
        if (idField == nullptr) return loadFailure(err.error, err.message);
        const auto id = idFromString(idField->asString());
        if (!id.has_value() || *id == kInvalidObjectId || *id > kMaxObjectId)
            return loadFailure(SerializationError::InvalidFieldType,
                               context + ": field 'id' is not a valid decimal ObjectId string");
        if (!registerId(*id, context, err)) return loadFailure(err.error, err.message);

        const JsonValue* name = requireField(entry, "name", JsonType::String, context, err);
        if (name == nullptr) return loadFailure(err.error, err.message);
        const JsonValue* source =
            requireField(entry, "sourcePath", JsonType::String, context, err);
        if (source == nullptr) return loadFailure(err.error, err.message);
        // The same refusal an Import gets (ADR-M22-003): a reference that names
        // no file can never resolve, so it is refused at the door rather than
        // at recompute time, a long way from the cause.
        if (source->asString().empty())
            return loadFailure(SerializationError::InvalidFieldType,
                               context + ": an instance names no part file");
        const JsonValue* bodyName =
            requireField(entry, "bodyName", JsonType::String, context, err);
        if (bodyName == nullptr) return loadFailure(err.error, err.message);
        const JsonValue* frameId = requireField(entry, "frameId", JsonType::String, context, err);
        if (frameId == nullptr) return loadFailure(err.error, err.message);
        const auto frame = idFromString(frameId->asString());
        if (!frame.has_value())
            return loadFailure(SerializationError::InvalidFieldType,
                               context + ": field 'frameId' is not a valid ObjectId");
        // The frame has to be IN THIS FILE. Checked here rather than left to
        // restoreInstance's throw, because a loader that throws is a loader a
        // caller cannot use.
        bool frameIsHere = false;
        for (const docjson::FrameData& candidate : frameData)
            if (candidate.id == *frame) frameIsHere = true;
        if (!frameIsHere)
            return loadFailure(SerializationError::UnknownDependencyId,
                               context + ": frameId " + idToString(*frame) +
                                   " is not a frame in this document");

        instanceData.push_back(InstanceData{*id, name->asString(), source->asString(),
                                            bodyName->asString(), *frame});
    }

    auto document = std::make_unique<AssemblyDocument>(documentId, documentName);
    // WHO OWNS THE ORIGIN ON LOAD. The constructor always makes one and the
    // file carries its own, so restoring naively would give the document two
    // frames named "Origin", one of them with an id nothing in the file points
    // at. The file wins: those are the frames every reference means.
    if (!frameData.empty())
        for (const ReferenceFrame* existing : document->frames())
            document->restoreRemoveObject(existing->id());
    docjson::restoreFramesAndConnectors(*document, frameData, connectorData);

    for (auto& instance : instanceData)
        document->restoreInstance(instance.id, std::move(instance.name), ComputeState::Dirty,
                                  std::move(instance.sourcePath), std::move(instance.bodyName),
                                  instance.frameId);

    // A loaded document starts with an EMPTY history: the load is not
    // something the user did, and "Undo" on a freshly opened file must not
    // take apart what was in it (ADR-M9-001).
    return AssemblyLoadResult{std::move(document), SerializationError::None, {}};
}

SaveResult saveAssemblyDocumentToFile(const AssemblyDocument& document, const std::string& path) {
    // VALIDATED BEFORE THE FILE IS OPENED. Opening it first truncates it, so a
    // refused save would already have destroyed the good version on disk --
    // which is the very thing ADR-M3-008 is about.
    if (const SaveResult invalid = validateSaveable(document); !invalid) return invalid;
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return SaveResult{SerializationError::IoError, "could not open '" + path + "' for writing"};
    return saveAssemblyDocument(document, out);
}

AssemblyLoadResult loadAssemblyDocumentFromFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return loadFailure(SerializationError::FileNotFound, "could not open '" + path + "'");
    return loadAssemblyDocument(in);
}

} // namespace paramcad
