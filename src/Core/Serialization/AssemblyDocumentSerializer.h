#pragma once

#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Serialization/SerializationError.h"

#include <iosfwd>
#include <memory>
#include <string>

namespace paramcad {

// THE SAME FILE FORMAT, HOLDING A DIFFERENT DOCUMENT (M23, ADR-M23-003).
//
// An assembly is written as `documentType: "Assembly"` in the same
// ParametricCAD JSON a part uses, at the same schema version, with the same
// header, the same decimal-string ObjectIds and the same frames and connectors
// arrays -- all of which come from DocumentJson rather than being spelled out
// again here. What is left is the one array that is genuinely new: `instances`.
//
// The rules the Part format lives by hold unchanged, because they are rules
// about the format and not about parts:
//
//   * NO GEOMETRY EVER CROSSES INTO THE FILE (ADR-M4-004). An instance stores
//     a path and a body NAME. Not a solid, and not a body index -- an index
//     would mean something different the day someone reordered the part;
//   * A DOCUMENT THAT SAVES MUST LOAD (ADR-M3-008). Every reference the loader
//     checks is checked at save time, so a broken assembly is refused while
//     the good file on disk is still intact, rather than after it has been
//     overwritten;
//   * ids are unique across the whole document, and an out-of-range one is
//     refused so the id generator can never wrap.
//
// Stateless free functions; no exceptions cross this API. On load, any error
// returns a null document with a structured error -- never a partially
// restored one.
struct AssemblyLoadResult {
    std::unique_ptr<AssemblyDocument> document;
    SerializationError error = SerializationError::None;
    std::string message;
    explicit operator bool() const noexcept {
        return error == SerializationError::None && document != nullptr;
    }
};

SaveResult saveAssemblyDocument(const AssemblyDocument& document, std::ostream& out);
AssemblyLoadResult loadAssemblyDocument(std::istream& in);

SaveResult saveAssemblyDocumentToFile(const AssemblyDocument& document, const std::string& path);
AssemblyLoadResult loadAssemblyDocumentFromFile(const std::string& path);

} // namespace paramcad
