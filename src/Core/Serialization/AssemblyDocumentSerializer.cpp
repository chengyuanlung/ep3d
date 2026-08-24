#include "Core/Serialization/AssemblyDocumentSerializer.h"

#include "Core/Serialization/DocumentJson.h"

#include <fstream>
#include <sstream>
#include <array>
#include <optional>
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
    for (const Mate* mate : document.mates()) {
        if (const SaveResult bad = claim(mate->id(), "mate"); !bad) return bad;
        // BOTH ENDS HAVE TO BE HERE. The loader checks it, so this checks it
        // first -- otherwise a document could save and then refuse to open,
        // which is the named worst case (ADR-M3-008).
        for (const ObjectId end : {mate->leadingInstanceId(), mate->followingInstanceId()})
            if (document.findInstance(end) == nullptr)
                return SaveResult{SerializationError::UnknownDependencyId,
                                  "mate '" + mate->name() +
                                      "' names an instance that is not in this document"};
        if (mate->leadingInstanceId() == mate->followingInstanceId())
            return SaveResult{SerializationError::InvalidDependency,
                              "mate '" + mate->name() + "' has the same instance at both ends"};
        // A connector NAME is not checked here and cannot be: it lives in the
        // part file, which may not even be present at save time. A name that
        // no longer resolves is a recompute failure that says so, not a
        // reason to refuse to write the document down.
        // A NUMBER ON A COMPONENT THIS MATE PINS can never be read back as
        // anything, so it is refused before it reaches the file rather than
        // when the file is opened again (ADR-M3-008).
        const MateFreedom freedom = mate->freedom();
        for (std::size_t c = 0; c < kMateComponentCount; ++c) {
            if (freedom.free[c] || mate->values()[c] == 0.0) continue;
            return SaveResult{SerializationError::InvalidFieldType,
                              "mate '" + mate->name() + "' is " +
                                  std::string(toString(mate->type())) +
                                  " and has no freedom " +
                                  std::string(toString(static_cast<MateComponent>(c))) +
                                  " to give a value to"};
        }
        // ...and the same for a limit: a bound on something that cannot move
        // is a control with nothing behind it.
        for (std::size_t c = 0; c < kMateComponentCount; ++c) {
            if (!mate->limits()[c].enabled) continue;
            if (!freedom.free[c])
                return SaveResult{SerializationError::InvalidFieldType,
                                  "mate '" + mate->name() + "' has a limit on a freedom it "
                                                            "does not have"};
            if (!(mate->limits()[c].min <= mate->limits()[c].max))
                return SaveResult{SerializationError::InvalidFieldType,
                                  "mate '" + mate->name() + "' has a limit whose minimum is "
                                                            "above its maximum"};
        }
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
        // v30. GROUNDED lives on the instance rather than in a list of its own:
        // a parallel array naming ids is a second place the set of instances
        // is written down, and the two would disagree the first time one was
        // deleted.
        entry.set("grounded", JsonValue::makeBool(document.isInstanceGrounded(instance->id())));
        instances.add(std::move(entry));
    }
    root.set("instances", std::move(instances));

    // v30 (M24). Each end is an instance id and a connector NAME -- the
    // connector itself lives in the part file and is reused by every instance
    // of it (roadmap §21), so there is no id here to point at.
    JsonValue mates = JsonValue::makeArray();
    for (const Mate* mate : document.mates()) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("id", JsonValue::makeString(idToString(mate->id())));
        entry.set("name", JsonValue::makeString(mate->name()));
        entry.set("type", JsonValue::makeString(std::string(toString(mate->type()))));
        entry.set("leadingInstanceId",
                  JsonValue::makeString(idToString(mate->leadingInstanceId())));
        entry.set("leadingConnector", JsonValue::makeString(mate->leadingConnector()));
        entry.set("followingInstanceId",
                  JsonValue::makeString(idToString(mate->followingInstanceId())));
        entry.set("followingConnector", JsonValue::makeString(mate->followingConnector()));
        // v31: ONE NUMBER PER COMPONENT, because a cylindrical mate turns and
        // slides and "the value" stopped being a single thing. RADIANS for the
        // rotations, millimetres for the translations -- the unit each freedom
        // has, written as the number it is. Degrees would be a second unit in a
        // file that has none.
        //
        // ONLY `values` is written, never the v30 `value` as well. Writing
        // both would put TWO answers to one question in the file, and the
        // first thing a hand edit would do is make them disagree -- which is
        // the seam this project spends its milestones removing. Backward
        // compatibility runs one way only: v30 files are still READ, and there
        // is no reader of this format but EP3D, so writing for an older one is
        // speculation with a cost.
        JsonValue values = JsonValue::makeArray();
        for (std::size_t c = 0; c < kMateComponentCount; ++c)
            values.add(JsonValue::makeNumber(mate->values()[c]));
        entry.set("values", std::move(values));
        entry.set("driven", JsonValue::makeBool(mate->isDriven()));

        // Limits, written only where there is one -- an array of six mostly
        // empty objects would make the common file harder to read for nothing.
        JsonValue limits = JsonValue::makeArray();
        for (std::size_t c = 0; c < kMateComponentCount; ++c) {
            if (!mate->limits()[c].enabled) continue;
            JsonValue one = JsonValue::makeObject();
            one.set("component", JsonValue::makeNumber(static_cast<double>(c)));
            one.set("min", JsonValue::makeNumber(mate->limits()[c].min));
            one.set("max", JsonValue::makeNumber(mate->limits()[c].max));
            limits.add(std::move(one));
        }
        entry.set("limits", std::move(limits));
        mates.add(std::move(entry));
    }
    root.set("mates", std::move(mates));
    return root;
}

struct InstanceData {
    ObjectId id = kInvalidObjectId;
    std::string name;
    std::string sourcePath;
    std::string bodyName;
    ObjectId frameId = kInvalidObjectId;
    bool grounded = false;
};

struct MateData {
    ObjectId id = kInvalidObjectId;
    std::string name;
    MateType type = MateType::Fastened;
    ObjectId leadingInstanceId = kInvalidObjectId;
    std::string leadingConnector;
    ObjectId followingInstanceId = kInvalidObjectId;
    std::string followingConnector;
    MateValues values{};
    bool driven = false;
    std::array<Mate::Limit, kMateComponentCount> limits{};
};

std::optional<MateType> mateTypeFromString(std::string_view text) {
    if (text == "Fastened") return MateType::Fastened;
    if (text == "Revolute") return MateType::Revolute;
    if (text == "Slider") return MateType::Slider;
    if (text == "Cylindrical") return MateType::Cylindrical;
    if (text == "Ball") return MateType::Ball;
    if (text == "Planar") return MateType::Planar;
    if (text == "Parallel") return MateType::Parallel;
    return std::nullopt;
}

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

        // v30. Absent in a v29 file, where nothing was grounded because nothing
        // could be -- so the default is false and those files load exactly as
        // they did.
        bool grounded = false;
        if (const JsonValue* groundedField = entry.find("grounded")) {
            if (groundedField->type() != JsonType::Bool)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": field 'grounded' is not a boolean");
            grounded = groundedField->asBool();
        }

        instanceData.push_back(InstanceData{*id, name->asString(), source->asString(),
                                            bodyName->asString(), *frame, grounded});
    }

    // v30 mates. Absent in a v29 file, which is why this is not required.
    std::vector<MateData> mateData;
    if (const JsonValue* matesField = root.find("mates")) {
        if (matesField->type() != JsonType::Array)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'mates' is not an array");
        for (std::size_t i = 0; i < matesField->items().size(); ++i) {
            const JsonValue& entry = matesField->items()[i];
            const std::string context = "mates[" + std::to_string(i) + "]";
            if (entry.type() != JsonType::Object)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": entry is not an object");
            MateData mate;
            const JsonValue* idField = requireField(entry, "id", JsonType::String, context, err);
            if (idField == nullptr) return loadFailure(err.error, err.message);
            const auto id = idFromString(idField->asString());
            if (!id.has_value() || *id == kInvalidObjectId || *id > kMaxObjectId)
                return loadFailure(
                    SerializationError::InvalidFieldType,
                    context + ": field 'id' is not a valid decimal ObjectId string");
            if (!registerId(*id, context, err)) return loadFailure(err.error, err.message);
            mate.id = *id;

            const JsonValue* name = requireField(entry, "name", JsonType::String, context, err);
            if (name == nullptr) return loadFailure(err.error, err.message);
            mate.name = name->asString();

            const JsonValue* type = requireField(entry, "type", JsonType::String, context, err);
            if (type == nullptr) return loadFailure(err.error, err.message);
            const auto parsedType = mateTypeFromString(type->asString());
            if (!parsedType.has_value())
                return loadFailure(SerializationError::InvalidEnumValue,
                                   context + ": unknown mate type '" + type->asString() + "'");
            mate.type = *parsedType;

            // BOTH ENDS, and both have to be instances IN THIS FILE. Checked
            // here rather than left to restoreMate's throw, because a loader
            // that throws is a loader a caller cannot use.
            struct End {
                const char* idKey;
                const char* connectorKey;
                ObjectId* into;
                std::string* connectorInto;
            };
            const End ends[2] = {{"leadingInstanceId", "leadingConnector",
                                  &mate.leadingInstanceId, &mate.leadingConnector},
                                 {"followingInstanceId", "followingConnector",
                                  &mate.followingInstanceId, &mate.followingConnector}};
            for (const End& end : ends) {
                const JsonValue* endId =
                    requireField(entry, end.idKey, JsonType::String, context, err);
                if (endId == nullptr) return loadFailure(err.error, err.message);
                const auto parsed = idFromString(endId->asString());
                if (!parsed.has_value())
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field '" + end.idKey +
                                           "' is not a valid ObjectId");
                bool isHere = false;
                for (const InstanceData& candidate : instanceData)
                    if (candidate.id == *parsed) isHere = true;
                if (!isHere)
                    return loadFailure(SerializationError::UnknownDependencyId,
                                       context + ": " + end.idKey + " " + idToString(*parsed) +
                                           " is not an instance in this document");
                *end.into = *parsed;

                const JsonValue* connector =
                    requireField(entry, end.connectorKey, JsonType::String, context, err);
                if (connector == nullptr) return loadFailure(err.error, err.message);
                // A mate that names no connector can never resolve. Refused at
                // the door, like an import with no path.
                if (connector->asString().empty())
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field '" + end.connectorKey +
                                           "' names no connector");
                *end.connectorInto = connector->asString();
            }
            if (mate.leadingInstanceId == mate.followingInstanceId)
                return loadFailure(SerializationError::InvalidDependency,
                                   context + ": both ends are the same instance");

            // v31 writes `values`; v30 wrote a single `value`. A v30 file is
            // read by putting its number on the first free component, which is
            // exactly what it meant -- every mate type v30 knew about has one
            // freedom.
            const MateFreedom freedom = FreedomOf(mate.type);
            if (const JsonValue* values = entry.find("values")) {
                if (values->type() != JsonType::Array ||
                    values->items().size() != kMateComponentCount)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'values' is not six numbers");
                for (std::size_t c = 0; c < kMateComponentCount; ++c) {
                    if (values->items()[c].type() != JsonType::Number)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           context + ": field 'values' is not six numbers");
                    mate.values[c] = values->items()[c].asNumber();
                }
            } else {
                const JsonValue* value =
                    requireField(entry, "value", JsonType::Number, context, err);
                if (value == nullptr) return loadFailure(err.error, err.message);
                for (std::size_t c = 0; c < kMateComponentCount; ++c)
                    if (freedom.free[c]) {
                        mate.values[c] = value->asNumber();
                        break;
                    }
                // A v30 fastened mate with a value was already refused when it
                // was written, so a file carrying one has been edited by hand.
                // Refused with the same sentence the facade uses.
                if (freedom.total() == 0 && value->asNumber() != 0.0)
                    return loadFailure(
                        SerializationError::InvalidFieldType,
                        context + ": a fastened mate has no freedom to give a value to");
            }
            for (std::size_t c = 0; c < kMateComponentCount; ++c) {
                if (freedom.free[c] || mate.values[c] == 0.0) continue;
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": this mate has no freedom " +
                                       std::string(toString(static_cast<MateComponent>(c))) +
                                       " to give a value to");
            }

            if (const JsonValue* driven = entry.find("driven")) {
                if (driven->type() != JsonType::Bool)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'driven' is not a boolean");
                mate.driven = driven->asBool();
            }
            if (const JsonValue* limits = entry.find("limits")) {
                if (limits->type() != JsonType::Array)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'limits' is not an array");
                for (const JsonValue& one : limits->items()) {
                    if (one.type() != JsonType::Object)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           context + ": a limit is not an object");
                    const JsonValue* which =
                        requireField(one, "component", JsonType::Number, context, err);
                    if (which == nullptr) return loadFailure(err.error, err.message);
                    const JsonValue* low =
                        requireField(one, "min", JsonType::Number, context, err);
                    if (low == nullptr) return loadFailure(err.error, err.message);
                    const JsonValue* high =
                        requireField(one, "max", JsonType::Number, context, err);
                    if (high == nullptr) return loadFailure(err.error, err.message);
                    const double index = which->asNumber();
                    if (index < 0.0 || index >= static_cast<double>(kMateComponentCount) ||
                        index != static_cast<double>(static_cast<int>(index)))
                        return loadFailure(SerializationError::InvalidFieldType,
                                           context + ": a limit names no component");
                    const std::size_t c = static_cast<std::size_t>(index);
                    // The same two rules the facade enforces, at the other
                    // door: a limit on a pinned component, or one whose
                    // minimum is above its maximum, can never be obeyed.
                    if (!freedom.free[c])
                        return loadFailure(SerializationError::InvalidFieldType,
                                           context + ": a limit on a freedom this mate does "
                                                     "not have");
                    if (!(low->asNumber() <= high->asNumber()))
                        return loadFailure(SerializationError::InvalidFieldType,
                                           context + ": a limit whose minimum is above its "
                                                     "maximum");
                    mate.limits[c] = Mate::Limit{true, low->asNumber(), high->asNumber()};
                }
            }
            mateData.push_back(std::move(mate));
        }
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

    for (auto& instance : instanceData) {
        document->restoreInstance(instance.id, std::move(instance.name), ComputeState::Dirty,
                                  std::move(instance.sourcePath), std::move(instance.bodyName),
                                  instance.frameId);
        // Through the restore path so the ground does not arrive as an undo
        // step (ADR-M9-001): a freshly opened document has nothing to undo.
        if (instance.grounded) document->restoreInstanceGrounded(instance.id);
    }
    // MATES AFTER INSTANCES, because a mate names two of them.
    for (auto& mate : mateData)
        document->restoreMateWithValues(mate.id, std::move(mate.name), mate.type,
                                        mate.leadingInstanceId, std::move(mate.leadingConnector),
                                        mate.followingInstanceId,
                                        std::move(mate.followingConnector), mate.values,
                                        mate.driven, mate.limits);

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
