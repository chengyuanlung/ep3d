#pragma once

#include <string>

namespace paramcad {

// HOW A SAVE OR A LOAD CAN GO WRONG, for every document type there is.
//
// Extracted from PartDocumentSerializer.h in M23, when the Assembly arrived:
// two enumerations of the same failures would let one serializer report
// MissingField where the other reported InvalidFieldType for the same broken
// file, and a caller cannot switch on two of anything.
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
    DuplicateId,
    UnknownDependencyId, // dependency endpoint is not a graph-node object in this file
    InvalidDependency    // dependency edge is a self-edge or would create a cycle
};


struct SaveResult {
    SerializationError error = SerializationError::None;
    std::string message;
    explicit operator bool() const noexcept { return error == SerializationError::None; }
};

} // namespace paramcad
