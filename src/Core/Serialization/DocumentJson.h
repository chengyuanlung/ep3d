#pragma once

#include "Core/Connector/Connector.h"
#include "Core/Document/ObjectId.h"
#include "Core/Geometry/MathTypes.h"
#include "Core/Serialization/JsonValue.h"
#include "Core/Serialization/SerializationError.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace paramcad {

class DocumentBase;

// WHAT EVERY DOCUMENT'S FILE HAS IN COMMON (M23, ADR-M23-003).
//
// The plan's P3 lists serialization first among the five things a second
// document type would otherwise duplicate, and it is the one where duplication
// is worst: two writers and two readers for the same header, the same ids and
// the same frames means four places a format can drift, and a file that saves
// under one and refuses to load under the other is ADR-M3-008's named worst
// case reached by a new route.
//
// So the pieces below are the format's shared vocabulary, and both the Part
// serializer and the Assembly serializer are built out of them rather than
// each spelling them out:
//
//   * the header -- format name, schema version, document type, id, name;
//   * an ObjectId as a decimal string (uint64 exceeds double precision);
//   * a Transform3D as seven numbers;
//   * the frames array and the connectors array, which belong to DocumentBase
//     and therefore to every document type there will ever be.
//
// Field validation returns a FieldError rather than throwing, matching the
// style the Part serializer already had; no exception crosses this API.
namespace docjson {

constexpr std::string_view kFormatName = "ParametricCAD";

// ONE VERSION COUNTER FOR ONE FORMAT (M23). Parts and assemblies are the same
// file format holding different documents, so a second counter would let a
// reader accept a v29 assembly and refuse a v29 part -- or, worse, disagree
// about what v29 MEANS.
constexpr int kSchemaVersion = 32;             // v32 adds named positions and exploded views (M26)
constexpr int kMinSupportedSchemaVersion = 1;  // v1 (no edges) and v2 files still load



struct FieldError {
    SerializationError error = SerializationError::None;
    std::string message;
    bool ok() const noexcept { return error == SerializationError::None; }
};

FieldError fieldError(SerializationError error, std::string message);

const JsonValue* requireField(const JsonValue& object, std::string_view key,
                              JsonType expectedType, const std::string& context,
                              FieldError& err);

std::string idToString(ObjectId id);
std::optional<ObjectId> idFromString(std::string_view text);

// The five fields every document file opens with. Written in one place and
// checked in one place, because a header that is written by two functions is
// two claims about what a file IS.
void writeHeader(JsonValue& root, std::string_view documentType, ObjectId id,
                 const std::string& name);
// Checks format, version and type, and hands back the id and name. `err`
// carries the refusal; the return is false on any of them.
bool readHeader(const JsonValue& root, std::string_view expectedType, FieldError& err,
                ObjectId& id, std::string& name);

// A Transform3D as seven numbers. Not a matrix: the stored form is the same
// translation + quaternion the type carries, so a save/load cannot renormalise
// or re-decompose it into something subtly different (M10, ADR-M10-002).
JsonValue transformToJson(const Transform3D& transform);
bool transformFromJson(const JsonValue& object, const std::string& context, FieldError& err,
                       Transform3D& out);

std::string_view toString(ConnectorRole role);
std::optional<ConnectorRole> connectorRoleFromString(std::string_view text);
std::string_view toString(ConnectorOwner owner);
std::optional<ConnectorOwner> connectorOwnerFromString(std::string_view text);

// What a frame and a connector are, on the way in. Plain data, because the
// loader reads EVERYTHING before it builds ANYTHING -- a half-restored
// document is never handed back (ADR-M3-002).
struct FrameData {
    ObjectId id = kInvalidObjectId;
    std::string name;
    ObjectId parentFrameId = kInvalidObjectId;
    Transform3D transform;
};

struct ConnectorData {
    ObjectId id = kInvalidObjectId;
    std::string name;
    ConnectorRole role = ConnectorRole::Generic;
    ObjectId frameId = kInvalidObjectId;
    ConnectorOwner owner = ConnectorOwner::PartDefinition;
};

JsonValue framesToJson(const DocumentBase& document);
JsonValue connectorsToJson(const DocumentBase& document);

// How the caller enforces its own document-wide id-uniqueness rule. Passed in
// rather than reimplemented here because the SET of ids differs by document
// type -- a Part's includes features, an Assembly's includes instances -- while
// the rule that an id appears once does not. Returns false and fills `err`.
using IdClaim = std::function<bool(ObjectId, const std::string& context, FieldError&)>;

// Absent arrays are not an error: every file written before v10 has no
// "frames" key, and those documents get the Origin frame the constructor makes
// exactly as they always did.
bool readFrames(const JsonValue& root, const IdClaim& claimId, FieldError& err,
                std::vector<FrameData>& out);
bool readConnectors(const JsonValue& root, const IdClaim& claimId, FieldError& err,
                    std::vector<ConnectorData>& out);

// Puts them back, in the order the references need: every frame flat first,
// then the parent links, then the connectors. Flat first because the file's
// frames are in creation order and a child may precede its parent, and
// restoreFrame refuses a parent it cannot see.
void restoreFramesAndConnectors(DocumentBase& document, std::vector<FrameData>& frames,
                                std::vector<ConnectorData>& connectors);

} // namespace docjson
} // namespace paramcad
