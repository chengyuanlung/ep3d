#pragma once

#include "Core/Document/PartDocument.h"
#include <iosfwd>
#include <memory>
#include <string>

namespace paramcad {

// Native JSON serialization of PartDocument, schema v1.
// Covered: document header, scalar parameters (expression round-trips as an
// uninterpreted string), bodies, and feature metadata (restored as
// PlaceholderFeature). Not serialized: frames, connectors, material, mass
// properties, sketches, dependency-graph contents (derived/deferred data).
// ObjectIds are stored as decimal strings (uint64 exceeds double precision);
// schema v1 accepts ids in [1, kMaxObjectId] only (larger values are rejected
// with InvalidFieldType so the id generator can never wrap), and every id in a
// document must be unique across the document, parameters, bodies, and
// features (violations are rejected with DuplicateId). Enums are stored as
// their exact enumerator-name strings.
// Stateless free functions; no exceptions cross this API.

enum class SerializationError {
    None,
    FileNotFound,
    IoError,
    MalformedJson,
    WrongFormat,
    UnsupportedSchemaVersion,
    WrongDocumentType,
    MissingField,
    InvalidFieldType,
    InvalidEnumValue,
    DuplicateId
};

struct SaveResult {
    SerializationError error = SerializationError::None;
    std::string message;
    explicit operator bool() const noexcept { return error == SerializationError::None; }
};

struct LoadResult {
    std::unique_ptr<PartDocument> document;
    SerializationError error = SerializationError::None;
    std::string message;
    explicit operator bool() const noexcept {
        return error == SerializationError::None && document != nullptr;
    }
};

// Canonical stream API. On load, any error returns a null document with a
// structured error (never a partially restored document). Unknown object keys
// are ignored; missing required fields and wrong JSON types are rejected.
SaveResult savePartDocument(const PartDocument& document, std::ostream& out);
LoadResult loadPartDocument(std::istream& in);

// File convenience wrappers over the stream API.
SaveResult savePartDocumentToFile(const PartDocument& document, const std::string& path);
LoadResult loadPartDocumentFromFile(const std::string& path);

} // namespace paramcad
